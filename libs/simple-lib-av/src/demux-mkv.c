// demux-mkv - Matroska video demuxer producing Annex-B H.264 access units. See demux-mkv.h.
#include "demux-mkv.h"
#include "ebml.h"
#include "dbg.h"                // logWarn
#include <string.h>
#include <stdlib.h>            // malloc / free

#define BLOCK_BUFFER_SIZE  (4 * 1024 * 1024)   // holds one raw block frame before conversion

// reads a big-endian unsigned integer of `length` bytes (1..8) at the current source position.
static uint64_t readUintBE(VideoSource *source, uint64_t length)
{
   uint64_t value = 0;
   for (uint64_t i = 0; i < length && i < 8; i++) { uint8_t b = 0; readVideoSource(source, &b, 1); value = (value << 8) | b; }
   return value;
}

// reads an EBML float (4 or 8 bytes big-endian) at the current source position.
static double readFloatBE(VideoSource *source, uint64_t size)
{
   uint64_t raw = readUintBE(source, size);
   if (size == 4) { uint32_t bits = (uint32_t)raw; float f; memcpy(&f, &bits, 4); return f; }
   if (size == 8) { double d; memcpy(&d, &raw, 8); return d; }
   return 0;
}

// seeks to `pos` and reads the element header there; returns 0 with *id/*size filled and the source
// left at the payload (*dataStart). -1 on seek/read failure or an unknown-size element.
static int readElementAt(MkvDemuxer *demuxer, uint64_t pos, uint32_t *id, uint64_t *size, uint64_t *dataStart)
{
   if (seekVideoSource(&demuxer->source, pos) != 0) return -1;
   int unknown = 0;
   if (readEbmlElement(&demuxer->source, id, size, &unknown) != 0 || unknown) return -1;
   *dataStart = getVideoSourcePosition(&demuxer->source);
   return 0;
}

// ============================================================================
// open: parse Info (timecode scale) + Tracks (video track, avcC, dimensions)
// ============================================================================

static void parseTrackEntry(MkvDemuxer *demuxer, uint64_t start, uint64_t end)
{
   int      trackNumber = 0, trackType = 0;
   char     codecId[32] = {0};
   int      width = 0, height = 0;
   int      audioRate = 0, audioChannels = 0;
   uint64_t frameDurationNs = 0;
   uint8_t  codecPrivate[256];
   int      codecPrivateSize = 0;

   uint64_t pos = start;
   int guard = 0;
   while (pos < end && guard++ < 64) {
      uint32_t id; uint64_t size, dataStart;
      if (readElementAt(demuxer, pos, &id, &size, &dataStart) != 0) return;

      if (id == EBML_ID_TRACKNUMBER) {
         trackNumber = (int)readUintBE(&demuxer->source, size);
      } else if (id == EBML_ID_TRACKTYPE) {
         trackType = (int)readUintBE(&demuxer->source, size);
      } else if (id == EBML_ID_DEFAULTDURATION) {
         frameDurationNs = readUintBE(&demuxer->source, size);   // ns per frame
      } else if (id == EBML_ID_CODECID) {
         uint64_t n = size < sizeof codecId - 1 ? size : sizeof codecId - 1;
         readVideoSource(&demuxer->source, codecId, n); codecId[n] = 0;
      } else if (id == EBML_ID_CODECPRIVATE) {
         codecPrivateSize = size < sizeof codecPrivate ? (int)size : (int)sizeof codecPrivate;
         readVideoSource(&demuxer->source, codecPrivate, codecPrivateSize);
      } else if (id == EBML_ID_VIDEO) {
         uint64_t vpos = dataStart, vend = dataStart + size;
         int vguard = 0;
         while (vpos < vend && vguard++ < 32) {
            uint32_t vid; uint64_t vsize, vdata;
            if (readElementAt(demuxer, vpos, &vid, &vsize, &vdata) != 0) break;
            if (vid == EBML_ID_PIXELWIDTH)       width  = (int)readUintBE(&demuxer->source, vsize);
            else if (vid == EBML_ID_PIXELHEIGHT) height = (int)readUintBE(&demuxer->source, vsize);
            vpos = vdata + vsize;
         }
      } else if (id == EBML_ID_AUDIO) {
         uint64_t apos = dataStart, aend = dataStart + size;
         int aguard = 0;
         while (apos < aend && aguard++ < 32) {
            uint32_t aid; uint64_t asize, adata;
            if (readElementAt(demuxer, apos, &aid, &asize, &adata) != 0) break;
            if (aid == EBML_ID_SAMPLINGFREQ)   audioRate     = (int)readFloatBE(&demuxer->source, asize);
            else if (aid == EBML_ID_CHANNELS)  audioChannels = (int)readUintBE(&demuxer->source, asize);
            apos = adata + asize;
         }
      }
      pos = dataStart + size;
   }

   // adopt the first AAC audio track we see (other audio codecs: video plays silent)
   if (trackType == 2 && demuxer->audioTrack == 0 && strncmp(codecId, "A_AAC", 5) == 0 && audioRate > 0) {
      demuxer->audioTrack    = trackNumber;
      demuxer->audioRate     = audioRate;
      demuxer->audioChannels = audioChannels;
      logInfo("[demux-mkv] aac track %d, %d Hz, %d ch\n", trackNumber, audioRate, audioChannels);
   }

   // adopt the first AVC video track we see
   int isAvc = (strncmp(codecId, "V_MPEG4/ISO/AVC", 15) == 0);
   if (trackType == 1 && isAvc && demuxer->videoTrack == 0) {
      if (parseAvcc(codecPrivate, codecPrivateSize, &demuxer->h264) != 0) return;
      demuxer->videoTrack      = trackNumber;
      demuxer->level           = codecPrivateSize >= 4 ? codecPrivate[3] : 0;   // AVCLevelIndication
      demuxer->width           = width;
      demuxer->height          = height;
      demuxer->frameDurationNs = frameDurationNs;
      logInfo("[demux-mkv] avc track %d, %dx%d, profile 0x%x level 0x%x, %d ref frames, %d fps\n", trackNumber, width, height,
              codecPrivateSize >= 2 ? codecPrivate[1] : 0, codecPrivateSize >= 4 ? codecPrivate[3] : 0, demuxer->h264.maxRefFrames,
              frameDurationNs ? (int)(1000000000ull / frameDurationNs) : 0);
   }
}

// reads the Cues element (keyframe seek index) into demuxer->cues.
static void parseCues(MkvDemuxer *demuxer, uint64_t start, uint64_t end)
{
   if (demuxer->cues) return;
   demuxer->cues = (MkvCue *)malloc(MAX_CUES * sizeof(MkvCue));
   if (!demuxer->cues) return;

   uint64_t pos = start;
   while (pos < end && demuxer->cueCount < MAX_CUES) {
      uint32_t id; uint64_t size, dataStart;
      if (readElementAt(demuxer, pos, &id, &size, &dataStart) != 0) break;

      if (id == EBML_ID_CUEPOINT) {
         uint64_t cueTime = 0, clusterPos = 0;
         uint64_t cpos = dataStart, cend = dataStart + size;
         int cguard = 0;
         while (cpos < cend && cguard++ < 16) {
            uint32_t cid; uint64_t csize, cdata;
            if (readElementAt(demuxer, cpos, &cid, &csize, &cdata) != 0) break;
            if (cid == EBML_ID_CUETIME) {
               cueTime = readUintBE(&demuxer->source, csize);
            } else if (cid == EBML_ID_CUETRACKPOS && clusterPos == 0) {
               uint64_t tpos = cdata, tend = cdata + csize;
               int tguard = 0;
               while (tpos < tend && tguard++ < 8) {
                  uint32_t tid; uint64_t tsize, tdata;
                  if (readElementAt(demuxer, tpos, &tid, &tsize, &tdata) != 0) break;
                  if (tid == EBML_ID_CUECLUSTERPOS) clusterPos = readUintBE(&demuxer->source, tsize);
                  tpos = tdata + tsize;
               }
            }
            cpos = cdata + csize;
         }
         if (clusterPos) {
            demuxer->cues[demuxer->cueCount].timeNs     = cueTime * demuxer->timecodeScale;
            demuxer->cues[demuxer->cueCount].clusterPos = demuxer->segmentStart + clusterPos;
            demuxer->cueCount++;
         }
      }
      pos = dataStart + size;
   }
   if (demuxer->cueCount == 0) { free(demuxer->cues); demuxer->cues = 0; }
}

// records where SeekHead says the Cues element (and any chained second SeekHead) lives.
static void parseSeekHead(MkvDemuxer *demuxer, uint64_t start, uint64_t end)
{
   uint64_t pos = start;
   int guard = 0;
   while (pos < end && guard++ < 1024) {   // rear SeekHeads can list hundreds of entries
      uint32_t id; uint64_t size, dataStart;
      if (readElementAt(demuxer, pos, &id, &size, &dataStart) != 0) return;

      if (id == EBML_ID_SEEK) {
         uint32_t targetId = 0; uint64_t targetPos = 0;
         uint64_t spos = dataStart, send = dataStart + size;
         int sguard = 0;
         while (spos < send && sguard++ < 8) {
            uint32_t sid; uint64_t ssize, sdata;
            if (readElementAt(demuxer, spos, &sid, &ssize, &sdata) != 0) break;
            if (sid == EBML_ID_SEEKID)            targetId  = (uint32_t)readUintBE(&demuxer->source, ssize);
            else if (sid == EBML_ID_SEEKPOSITION) targetPos = readUintBE(&demuxer->source, ssize);
            spos = sdata + ssize;
         }
         if (targetId == EBML_ID_CUES && targetPos) demuxer->cuesPos = demuxer->segmentStart + targetPos;
         else if (targetId == EBML_ID_SEEKHEAD && targetPos) demuxer->chainedSeekHeadPos = demuxer->segmentStart + targetPos;
      }
      pos = dataStart + size;
   }
}

// walks Info + Tracks (both precede the clusters) to fill the demuxer's track metadata.
static void parseMetadata(MkvDemuxer *demuxer, uint64_t segStart, uint64_t segEnd)
{
   uint64_t pos = segStart;
   int guard = 0;
   while (pos < segEnd && guard++ < 256) {
      if (seekVideoSource(&demuxer->source, pos) != 0) return;
      uint32_t id; uint64_t size; int unknown = 0;
      if (readEbmlElement(&demuxer->source, &id, &size, &unknown) != 0) return;
      uint64_t dataStart = getVideoSourcePosition(&demuxer->source);

      if (id == EBML_ID_CLUSTER) return;   // media started; metadata is complete
      if (id == EBML_ID_SEEKHEAD) {
         parseSeekHead(demuxer, dataStart, dataStart + size);
      } else if (id == EBML_ID_CUES) {
         parseCues(demuxer, dataStart, dataStart + size);
      } else if (id == EBML_ID_INFO) {
         double durationTicks = 0;   // in timecode ticks; scaled to ns after the walk (order-independent)
         uint64_t ipos = dataStart, iend = dataStart + size;
         int iguard = 0;
         while (ipos < iend && iguard++ < 64) {
            uint32_t iid; uint64_t isize, idata;
            if (readElementAt(demuxer, ipos, &iid, &isize, &idata) != 0) break;
            if (iid == EBML_ID_TIMECODESCALE)  demuxer->timecodeScale = readUintBE(&demuxer->source, isize);
            else if (iid == EBML_ID_DURATION)  durationTicks = readFloatBE(&demuxer->source, isize);
            ipos = idata + isize;
         }
         demuxer->durationNs = (uint64_t)(durationTicks * (double)demuxer->timecodeScale);
      } else if (id == EBML_ID_TRACKS) {
         uint64_t tpos = dataStart, tend = dataStart + size;
         int tguard = 0;
         while (tpos < tend && tguard++ < 64) {
            uint32_t tid; uint64_t tsize, tdata;
            if (readElementAt(demuxer, tpos, &tid, &tsize, &tdata) != 0) break;
            if (tid == EBML_ID_TRACKENTRY) parseTrackEntry(demuxer, tdata, tdata + tsize);
            tpos = tdata + tsize;
         }
      }
      if (unknown) return;
      pos = dataStart + size;
   }
}

int openMkvDemuxer(MkvDemuxer *demuxer, VideoSource *source)
{
   memset(demuxer, 0, sizeof *demuxer);
   demuxer->timecodeScale = 1000000;   // Matroska default: 1 ms per tick

   demuxer->source = *source;            // adopt the already-open source (caller opened + sniffed it)
   memset(source, 0, sizeof *source);    // ownership moved here; caller must not close it

   // EBML header, then Segment (whose size may legitimately be unknown)
   uint32_t id; uint64_t size, dataStart; int unknown = 0;
   if (readElementAt(demuxer, 0, &id, &size, &dataStart) != 0 || id != EBML_ID_HEADER) goto fail;
   if (seekVideoSource(&demuxer->source, dataStart + size) != 0) goto fail;
   if (readEbmlElement(&demuxer->source, &id, &size, &unknown) != 0 || id != EBML_ID_SEGMENT) goto fail;

   uint64_t segStart = getVideoSourcePosition(&demuxer->source);
   demuxer->segmentStart = segStart;
   demuxer->segmentEnd   = unknown ? getVideoSourceSize(&demuxer->source) : segStart + size;

   parseMetadata(demuxer, segStart, demuxer->segmentEnd);
   if (demuxer->videoTrack == 0 || !demuxer->h264.valid) goto fail;   // no decodable AVC track

   // some muxers write only a tiny SeekHead up front whose sole entry points at a second SeekHead
   // near the end of the file, and that one holds the Cues entry; follow the chain one hop
   if (!demuxer->cuesPos && demuxer->chainedSeekHeadPos &&
       readElementAt(demuxer, demuxer->chainedSeekHeadPos, &id, &size, &dataStart) == 0 && id == EBML_ID_SEEKHEAD)
      parseSeekHead(demuxer, dataStart, dataStart + size);

   // the Cues (seek index) usually live after the clusters; SeekHead told us where
   if (!demuxer->cues && demuxer->cuesPos &&
       readElementAt(demuxer, demuxer->cuesPos, &id, &size, &dataStart) == 0 && id == EBML_ID_CUES)
      parseCues(demuxer, dataStart, dataStart + size);

   if (demuxer->cueCount) logInfo("[demux-mkv] cue index: %d entries\n", demuxer->cueCount);
   else logWarn("[demux-mkv] no cue index (missing or truncated Cues), seeking will scan clusters\n");

   for (int i = 0; i < AU_BUFFER_COUNT; i++) {
      demuxer->auBuffers[i] = (uint8_t *)malloc(AU_BUFFER_SIZE);
      if (!demuxer->auBuffers[i]) goto fail;
   }
   demuxer->blockBuffer = (uint8_t *)malloc(BLOCK_BUFFER_SIZE);
   if (!demuxer->blockBuffer) goto fail;
   if (demuxer->audioTrack && createAudioAuQueue(&demuxer->audioQueue) != 0)
      demuxer->audioTrack = 0;   // no queue: play silent rather than fail
   demuxer->auCapacity    = AU_BUFFER_SIZE;
   demuxer->blockCapacity = BLOCK_BUFFER_SIZE;

   demuxer->pos = segStart;   // block reads walk the segment from the top, skipping non-clusters
   return 0;

fail:
   closeMkvDemuxer(demuxer);
   return -1;
}

// parses an EBML variable-length integer from a memory buffer; returns bytes consumed (0 on error).
static int parseVintFromBuffer(const uint8_t *data, int size, uint64_t *value)
{
   if (size < 1) return 0;
   int length = 1;
   uint8_t mask = 0x80;
   while (length <= 8 && !(data[0] & mask)) { mask >>= 1; length++; }
   if (length > 8 || length > size) return 0;
   uint64_t v = data[0] & (mask - 1);
   for (int i = 1; i < length; i++) v = (v << 8) | data[i];
   *value = v;
   return length;
}

// splits one (possibly laced) audio block into raw AAC frames and queues each with its pts.
// Matroska lacing: a frame-count byte, then per-frame sizes as 255-runs (Xiph), nothing (fixed,
// equal sizes), or a vint + signed vint deltas (EBML); the last frame takes the remaining bytes.
static void enqueueAudioBlock(MkvDemuxer *demuxer, uint64_t start, uint64_t end, uint8_t flags, int16_t relativeTimecode)
{
   int blockSize = (int)(end - start);
   if (blockSize <= 0 || blockSize > demuxer->blockCapacity) return;
   if (readVideoSourceAt(&demuxer->source, start, demuxer->blockBuffer, blockSize) != 0) return;

   int64_t timecode = (int64_t)demuxer->clusterTimecode + relativeTimecode;
   if (timecode < 0) timecode = 0;
   uint64_t pts = (uint64_t)timecode * demuxer->timecodeScale;
   uint64_t frameDurationNs = 1024ull * 1000000000ull / (uint64_t)demuxer->audioRate;   // AAC-LC: 1024 samples per frame

   const uint8_t *cursor = demuxer->blockBuffer;
   int remaining = blockSize;
   int lacing = (flags >> 1) & 0x03;

   if (lacing == 0) { enqueueAudioAu(&demuxer->audioQueue, cursor, remaining, pts); return; }

   // section: parse the lace sizes
   int frameCount = cursor[0] + 1;
   cursor++; remaining--;
   int sizes[64];
   if (frameCount > 64) return;

   if (lacing == 1) {                        // Xiph: 255-run sizes for all but the last frame
      for (int i = 0; i < frameCount - 1; i++) {
         int frameSize = 0;
         while (remaining > 0 && cursor[0] == 255) { frameSize += 255; cursor++; remaining--; }
         if (remaining <= 0) return;
         frameSize += cursor[0]; cursor++; remaining--;
         sizes[i] = frameSize;
      }
   } else if (lacing == 3) {                 // EBML: first size a vint, then signed vint deltas
      uint64_t value;
      int used = parseVintFromBuffer(cursor, remaining, &value);
      if (!used) return;
      cursor += used; remaining -= used;
      sizes[0] = (int)value;
      for (int i = 1; i < frameCount - 1; i++) {
         used = parseVintFromBuffer(cursor, remaining, &value);
         if (!used) return;
         int64_t delta = (int64_t)value - ((1ll << (7 * used - 1)) - 1);   // signed vint
         cursor += used; remaining -= used;
         sizes[i] = sizes[i - 1] + (int)delta;
         if (sizes[i] <= 0) return;   // corrupt lace sizes: don't walk backwards through the block
      }
   } else {                                  // fixed: equal sizes, no size data
      if (remaining % frameCount != 0) return;
      for (int i = 0; i < frameCount - 1; i++) sizes[i] = remaining / frameCount;
   }

   // section: queue each frame; the last takes whatever is left
   int consumed = 0;
   for (int i = 0; i < frameCount - 1; i++) consumed += sizes[i];
   if (consumed > remaining) return;
   sizes[frameCount - 1] = remaining - consumed;

   for (int i = 0; i < frameCount; i++) {
      enqueueAudioAu(&demuxer->audioQueue, cursor, sizes[i], pts + (uint64_t)i * frameDurationNs);
      cursor += sizes[i];
   }
}

// ============================================================================
// read: walk clusters, convert one video block to an Annex-B access unit
// ============================================================================

// parses a (Simple)Block in [start, end) into `au`. returns 1 if it produced a video AU, 0 if the
// block is for another track / laced / empty, -1 on a hard error.
static int parseBlock(MkvDemuxer *demuxer, uint64_t start, uint64_t end, VideoAu *au)
{
   if (seekVideoSource(&demuxer->source, start) != 0) return -1;

   uint64_t trackNumber;
   if (readEbmlVint(&demuxer->source, &trackNumber, 0, 0) == 0) return -1;

   uint8_t timecodeBytes[2], flags;
   if (readVideoSource(&demuxer->source, timecodeBytes, 2) != 2) return -1;
   if (readVideoSource(&demuxer->source, &flags, 1) != 1) return -1;
   int16_t relativeTimecode = (int16_t)((timecodeBytes[0] << 8) | timecodeBytes[1]);

   uint64_t frameStart = getVideoSourcePosition(&demuxer->source);
   if (demuxer->audioTrack && (int)trackNumber == demuxer->audioTrack) {
      enqueueAudioBlock(demuxer, frameStart, end, flags, relativeTimecode);
      return 0;
   }
   if ((int)trackNumber != demuxer->videoTrack) return 0;

   if ((flags >> 1) & 0x03) {   // lacing: multiple frames per block; video keyframes are unlaced
      if (!demuxer->lacingWarned) { logWarn("[demux-mkv] laced block skipped (unsupported)\n"); demuxer->lacingWarned = 1; }
      return 0;
   }

   int frameSize = (int)(end - frameStart);
   if (frameSize <= 0 || frameSize > demuxer->blockCapacity) return 0;
   if (readVideoSourceAt(&demuxer->source, frameStart, demuxer->blockBuffer, frameSize) != 0) return -1;

   int64_t timecode = (int64_t)demuxer->clusterTimecode + relativeTimecode;
   if (timecode < 0) timecode = 0;

   uint8_t *auBuffer = demuxer->auBuffers[demuxer->auIndex];   // alternate so the previous AU stays valid in the decoder
   if (!buildVideoAu(&demuxer->h264, demuxer->blockBuffer, frameSize, 0, auBuffer, demuxer->auCapacity,
                     (uint64_t)timecode * demuxer->timecodeScale, au)) return 0;
   demuxer->auIndex = (demuxer->auIndex + 1) % AU_BUFFER_COUNT;
   return 1;
}

int readMkvVideoAu(MkvDemuxer *demuxer, VideoAu *au)
{
   int guard = 0;
   while (demuxer->pos < demuxer->segmentEnd && guard++ < 100000) {
      if (seekVideoSource(&demuxer->source, demuxer->pos) != 0) return -1;
      uint32_t id; uint64_t size; int unknown = 0;
      if (readEbmlElement(&demuxer->source, &id, &size, &unknown) != 0) return 0;
      uint64_t dataStart = getVideoSourcePosition(&demuxer->source);

      if (id == EBML_ID_CLUSTER) {           // enter the cluster; its children follow
         demuxer->clusterTimecode = 0;
         demuxer->pos = dataStart;
         continue;
      }
      if (id == EBML_ID_TIMECODE) {
         demuxer->clusterTimecode = readUintBE(&demuxer->source, size);
         demuxer->pos = dataStart + size;
         continue;
      }
      if (id == EBML_ID_SIMPLEBLOCK) {
         int got = parseBlock(demuxer, dataStart, dataStart + size, au);
         demuxer->pos = dataStart + size;
         if (got == 1) return 1;
         if (got < 0) return -1;
         continue;
      }
      if (id == EBML_ID_BLOCKGROUP) {        // enter to find the Block child
         uint64_t bpos = dataStart, bend = dataStart + size;
         int result = 0, bguard = 0;
         while (bpos < bend && bguard++ < 32) {
            uint32_t bid; uint64_t bsize, bdata;
            if (readElementAt(demuxer, bpos, &bid, &bsize, &bdata) != 0) break;
            if (bid == EBML_ID_BLOCK) { result = parseBlock(demuxer, bdata, bdata + bsize, au); if (result != 0) break; }
            bpos = bdata + bsize;
         }
         demuxer->pos = dataStart + size;
         if (result == 1) return 1;
         if (result < 0) return -1;
         continue;
      }
      if (unknown) return 0;
      demuxer->pos = dataStart + size;      // skip anything else (SeekHead, Info, Tracks, Cues, ...)
   }
   return 0;
}

// ============================================================================
// seek
// ============================================================================

// grows the lazily-built cluster index (the fallback when the file has no Cues) until it covers
// targetNs, the file ends, or the index is full. Walking cluster headers costs one small read per
// cluster, so indexing deep into a large file takes a moment the first time; later seeks reuse it.
static void growClusterIndex(MkvDemuxer *demuxer, uint64_t targetNs)
{
   if (!demuxer->clusterIndex) {
      demuxer->clusterIndex = (MkvCue *)malloc(MAX_CUES * sizeof(MkvCue));
      if (!demuxer->clusterIndex) return;
      demuxer->clusterIndexNextPos = demuxer->segmentStart;
   }
   while (demuxer->clusterIndexCount < MAX_CUES && demuxer->clusterIndexNextPos < demuxer->segmentEnd) {
      if (demuxer->clusterIndexCount && demuxer->clusterIndex[demuxer->clusterIndexCount - 1].timeNs > targetNs) return;

      // read failures end the index: a truncated file, or an unsized cluster we can't skip over
      uint32_t id; uint64_t size, dataStart;
      if (readElementAt(demuxer, demuxer->clusterIndexNextPos, &id, &size, &dataStart) != 0) return;

      if (id == EBML_ID_CLUSTER) {
         // the cluster's Timecode child precedes its blocks; tolerate a leading CRC-32
         uint64_t childPos = dataStart;
         for (int child = 0; child < 4; child++) {
            uint32_t childId; uint64_t childSize, childData;
            if (readElementAt(demuxer, childPos, &childId, &childSize, &childData) != 0) break;
            if (childId == EBML_ID_TIMECODE) {
               MkvCue *entry = &demuxer->clusterIndex[demuxer->clusterIndexCount++];
               entry->timeNs     = readUintBE(&demuxer->source, childSize) * demuxer->timecodeScale;
               entry->clusterPos = demuxer->clusterIndexNextPos;
               break;
            }
            childPos = childData + childSize;
         }
      }
      demuxer->clusterIndexNextPos = dataStart + size;
   }
}

uint64_t seekMkvDemuxer(MkvDemuxer *demuxer, uint64_t targetNs)
{
   clearAudioAuQueue(&demuxer->audioQueue);   // consumer is parked: discard queued audio

   // prefer the file's cue index; without one, use the lazily-grown index of cluster start times
   // (landing anywhere is fine: decode skips forward to the next keyframe after every seek)
   const MkvCue *index = demuxer->cues;
   int count = demuxer->cueCount;
   if (!count) {
      growClusterIndex(demuxer, targetNs);
      index = demuxer->clusterIndex;
      count = demuxer->clusterIndexCount;
   }

   int best = -1;
   for (int i = 0; i < count && index[i].timeNs <= targetNs; i++) best = i;

   demuxer->pos = best >= 0 ? index[best].clusterPos : demuxer->segmentStart;
   demuxer->clusterTimecode = 0;
   return best >= 0 ? index[best].timeNs : 0;
}

void closeMkvDemuxer(MkvDemuxer *demuxer)
{
   for (int i = 0; i < AU_BUFFER_COUNT; i++) { free(demuxer->auBuffers[i]); demuxer->auBuffers[i] = 0; }
   free(demuxer->blockBuffer);
   free(demuxer->cues);
   free(demuxer->clusterIndex);
   destroyAudioAuQueue(&demuxer->audioQueue);
   demuxer->blockBuffer = 0;
   demuxer->cues = 0;
   demuxer->clusterIndex = 0;
   closeVideoSource(&demuxer->source);
}
