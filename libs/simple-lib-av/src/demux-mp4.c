// demux-mp4 - MP4 (ISOBMFF) video demuxer producing Annex-B H.264 access units. See demux-mp4.h.
#include "demux-mp4.h"
#include "mp4.h"
#include "dbg.h"
#include <string.h>
#include <stdlib.h>

#define MAX_SAMPLES        1000000               // per track (~11 h of 48 kHz AAC frames)
#define MAX_TABLE_BYTES    (16 * 1024 * 1024)    // sanity cap when loading one stbl table
#define SAMPLE_BUFFER_SIZE (4 * 1024 * 1024)     // holds one raw length-prefixed sample
#define AUDIO_LEAD_NS      1000000000ull         // queue audio up to this far ahead of video

// converts track-timescale ticks to nanoseconds without overflowing 64 bits
static uint64_t ticksToNs(uint64_t ticks, uint64_t timescale)
{
   if (!timescale) return 0;
   return (ticks / timescale) * 1000000000ull + (ticks % timescale) * 1000000000ull / timescale;
}

// ============================================================================
// open: walk moov, flatten each track's stbl tables into a sample array
// ============================================================================

// finds `type` in [start, end) and loads its whole payload into a fresh heap buffer (NULL when
// absent, oversized or unreadable). *outSize gets the payload length.
static uint8_t *loadChildBox(VideoSource *source, uint64_t start, uint64_t end, uint32_t type, uint32_t *outSize)
{
   *outSize = 0;
   uint64_t payloadStart, payloadEnd;
   if (findMp4ChildBox(source, start, end, type, &payloadStart, &payloadEnd) != 0) return 0;
   uint64_t size = payloadEnd - payloadStart;
   if (size < 8 || size > MAX_TABLE_BYTES) return 0;
   uint8_t *data = (uint8_t *)malloc(size);
   if (!data) return 0;
   if (readVideoSourceAt(source, payloadStart, data, size) != 0) { free(data); return 0; }
   *outSize = (uint32_t)size;
   return data;
}

// clamps a fullbox table's declared entry count to what its payload actually holds
static uint32_t getTableEntryCount(const uint8_t *table, uint32_t tableSize, uint32_t entryBytes)
{
   if (!table || tableSize < 8) return 0;
   uint32_t declared  = readU32BE(table + 4);
   uint32_t available = (tableSize - 8) / entryBytes;
   return declared < available ? declared : available;
}

// flattens one track's stbl tables into a sample array in decode order. returns the array with
// *outCount filled, or NULL. For video, *outFrameDurationNs gets the first stts delta.
static Mp4Sample *buildSampleTable(VideoSource *source, uint64_t stblStart, uint64_t stblEnd,
                                   uint64_t timescale, int isVideo, int *outCount, uint64_t *outFrameDurationNs)
{
   Mp4Sample *samples = 0;
   *outCount = 0;

   // section: load the raw tables to heap (one region each, so the source's read-ahead isn't thrashed)
   uint32_t sttsSize = 0, cttsSize = 0, stssSize = 0, stscSize = 0, stszSize = 0, stcoSize = 0;
   uint8_t *ctts = 0, *stss = 0;
   uint8_t *stts = loadChildBox(source, stblStart, stblEnd, FOURCC('s','t','t','s'), &sttsSize);
   uint8_t *stsc = loadChildBox(source, stblStart, stblEnd, FOURCC('s','t','s','c'), &stscSize);
   uint8_t *stsz = loadChildBox(source, stblStart, stblEnd, FOURCC('s','t','s','z'), &stszSize);
   uint8_t *stco = loadChildBox(source, stblStart, stblEnd, FOURCC('s','t','c','o'), &stcoSize);
   int co64 = 0;
   if (!stco) { stco = loadChildBox(source, stblStart, stblEnd, FOURCC('c','o','6','4'), &stcoSize); co64 = 1; }
   if (isVideo) {
      ctts = loadChildBox(source, stblStart, stblEnd, FOURCC('c','t','t','s'), &cttsSize);   // optional
      stss = loadChildBox(source, stblStart, stblEnd, FOURCC('s','t','s','s'), &stssSize);   // optional
   }
   if (!stts || !stsc || !stsz || !stco) goto done;

   // section: sample sizes (stsz: constant size, or one u32 per sample)
   if (stszSize < 12) goto done;
   uint32_t constantSize = readU32BE(stsz + 4);
   uint32_t sampleCount  = readU32BE(stsz + 8);
   if (!sampleCount || sampleCount > MAX_SAMPLES) goto done;
   if (!constantSize && (stszSize - 12) / 4 < sampleCount) goto done;
   samples = (Mp4Sample *)malloc(sampleCount * sizeof(Mp4Sample));
   if (!samples) goto done;
   for (uint32_t i = 0; i < sampleCount; i++)
      samples[i].size = constantSize ? constantSize : readU32BE(stsz + 12 + 4 * i);

   // section: presentation times (stts runs of decode-time deltas + optional ctts composition offsets)
   uint32_t sttsCount = getTableEntryCount(stts, sttsSize, 8);
   uint32_t cttsCount = getTableEntryCount(ctts, cttsSize, 8);
   uint64_t dts = 0;
   uint32_t sttsEntry = 0, sttsLeft = 0, sttsDelta = 0;
   uint32_t cttsEntry = 0, cttsLeft = 0;
   int32_t  cttsOffset = 0;
   for (uint32_t i = 0; i < sampleCount; i++) {
      if (!sttsLeft && sttsEntry < sttsCount) {
         sttsLeft  = readU32BE(stts + 8 + sttsEntry * 8);
         sttsDelta = readU32BE(stts + 12 + sttsEntry * 8);
         sttsEntry++;
      }
      if (!cttsLeft && cttsEntry < cttsCount) {
         cttsLeft   = readU32BE(ctts + 8 + cttsEntry * 8);
         cttsOffset = (int32_t)readU32BE(ctts + 12 + cttsEntry * 8);
         cttsEntry++;
      }
      int64_t compositionTicks = (int64_t)dts + cttsOffset;
      samples[i].ptsNs    = compositionTicks > 0 ? ticksToNs((uint64_t)compositionTicks, timescale) : 0;
      samples[i].keyframe = stss ? 0 : 1;   // no stss = every sample is a sync sample
      if (sttsLeft) { dts += sttsDelta; sttsLeft--; }
      if (cttsLeft) cttsLeft--;
   }
   if (outFrameDurationNs && sttsCount) *outFrameDurationNs = ticksToNs(readU32BE(stts + 12), timescale);

   // section: keyframes (stss lists sync samples by 1-based number)
   uint32_t stssCount = getTableEntryCount(stss, stssSize, 4);
   for (uint32_t i = 0; i < stssCount; i++) {
      uint32_t sampleNumber = readU32BE(stss + 8 + 4 * i);
      if (sampleNumber >= 1 && sampleNumber <= sampleCount) samples[sampleNumber - 1].keyframe = 1;
   }

   // section: file offsets (stsc maps chunks to samples-per-chunk, stco/co64 holds chunk offsets)
   uint32_t chunkCount = getTableEntryCount(stco, stcoSize, co64 ? 8 : 4);
   uint32_t stscCount  = getTableEntryCount(stsc, stscSize, 12);
   uint32_t sampleIndex = 0, stscEntry = 0, samplesPerChunk = 1;
   for (uint32_t chunk = 1; chunk <= chunkCount && sampleIndex < sampleCount; chunk++) {
      while (stscEntry < stscCount && readU32BE(stsc + 8 + stscEntry * 12) <= chunk) {   // first_chunk
         samplesPerChunk = readU32BE(stsc + 12 + stscEntry * 12);
         stscEntry++;
      }
      uint64_t offset = co64 ? readU64BE(stco + 8 + (chunk - 1) * 8) : readU32BE(stco + 8 + (chunk - 1) * 4);
      for (uint32_t s = 0; s < samplesPerChunk && sampleIndex < sampleCount; s++) {
         samples[sampleIndex].offset = offset;
         offset += samples[sampleIndex].size;
         sampleIndex++;
      }
   }
   if (sampleIndex < sampleCount) sampleCount = sampleIndex;   // tolerate truncated chunk tables

   if (!sampleCount) { free(samples); samples = 0; }
   *outCount = (int)sampleCount;

done:
   free(stts); free(ctts); free(stss); free(stsc); free(stsz); free(stco);
   return samples;
}

// reads one trak's handler, timescale and sample description; adopts the first decodable H.264
// video track and the first AAC (mp4a) audio track.
static void parseMp4Trak(Mp4Demuxer *demuxer, uint64_t trakStart, uint64_t trakEnd)
{
   VideoSource *source = &demuxer->source;
   uint64_t mdiaStart, mdiaEnd;
   if (findMp4ChildBox(source, trakStart, trakEnd, FOURCC('m','d','i','a'), &mdiaStart, &mdiaEnd) != 0) return;

   // mdhd: track timescale (version 1 widens the surrounding times to 64-bit, shifting it by 8)
   uint64_t mdhdStart, mdhdEnd;
   if (findMp4ChildBox(source, mdiaStart, mdiaEnd, FOURCC('m','d','h','d'), &mdhdStart, &mdhdEnd) != 0) return;
   uint8_t mdhd[24];
   if (readVideoSourceAt(source, mdhdStart, mdhd, sizeof mdhd) != 0) return;
   uint64_t timescale = mdhd[0] == 1 ? readU32BE(mdhd + 20) : readU32BE(mdhd + 12);
   if (!timescale) return;

   // hdlr: 'vide' or 'soun'
   uint64_t hdlrStart, hdlrEnd;
   if (findMp4ChildBox(source, mdiaStart, mdiaEnd, FOURCC('h','d','l','r'), &hdlrStart, &hdlrEnd) != 0) return;
   uint8_t hdlr[12];
   if (readVideoSourceAt(source, hdlrStart, hdlr, sizeof hdlr) != 0) return;
   uint32_t handler = readU32BE(hdlr + 8);

   // stbl and the first entry of its sample description
   uint64_t s = mdiaStart, e = mdiaEnd;
   if (findMp4ChildBox(source, s, e, FOURCC('m','i','n','f'), &s, &e) != 0) return;
   if (findMp4ChildBox(source, s, e, FOURCC('s','t','b','l'), &s, &e) != 0) return;
   uint64_t stblStart = s, stblEnd = e;
   if (findMp4ChildBox(source, stblStart, stblEnd, FOURCC('s','t','s','d'), &s, &e) != 0) return;
   uint64_t entryPos = s + 8;   // stsd payload: version/flags + entry_count, then the first sample entry
   uint32_t entryType; uint64_t entrySize, entryHeaderLen;
   if (readMp4Box(source, entryPos, &entryType, &entrySize, &entryHeaderLen) != 0) return;
   uint64_t entryEnd = entryPos + entrySize < e ? entryPos + entrySize : e;

   if (handler == FOURCC('v','i','d','e') && !demuxer->h264.valid &&
       (entryType == FOURCC('a','v','c','1') || entryType == FOURCC('a','v','c','3'))) {
      // VisualSampleEntry: width/height at payload offset 24; child boxes (avcC) after its 78-byte body
      uint8_t wh[4];
      if (readVideoSourceAt(source, entryPos + 8 + 24, wh, 4) != 0) return;
      uint64_t cfgStart, cfgEnd;
      if (findMp4ChildBox(source, entryPos + 8 + 78, entryEnd, FOURCC('a','v','c','C'), &cfgStart, &cfgEnd) != 0) return;
      uint8_t avcc[256];   // avcC record (SPS/PPS)
      int n = (int)(cfgEnd - cfgStart);
      if (n > (int)sizeof avcc) n = sizeof avcc;
      if (n < 4 || readVideoSourceAt(source, cfgStart, avcc, n) != 0) return;
      if (parseAvcc(avcc, n, &demuxer->h264) != 0) return;
      demuxer->videoSamples = buildSampleTable(source, stblStart, stblEnd, timescale, 1,
                                               &demuxer->videoSampleCount, &demuxer->frameDurationNs);
      demuxer->level  = avcc[3];   // AVCLevelIndication
      demuxer->width  = readU16BE(wh);
      demuxer->height = readU16BE(wh + 2);
      logInfo("[demux-mp4] avc track, %dx%d, profile 0x%x level 0x%x, %d ref frames, %d samples, %d fps\n",
              demuxer->width, demuxer->height, avcc[1], avcc[3], demuxer->h264.maxRefFrames, demuxer->videoSampleCount,
              demuxer->frameDurationNs ? (int)(1000000000ull / demuxer->frameDurationNs) : 0);
   } else if (handler == FOURCC('s','o','u','n') && !demuxer->hasAudio && entryType == FOURCC('m','p','4','a')) {
      // AudioSampleEntry: channel count at payload offset 16, sample rate (16.16 fixed) at 24
      uint8_t entry[28];
      if (readVideoSourceAt(source, entryPos + 8, entry, sizeof entry) != 0) return;
      int channels = readU16BE(entry + 16);
      int rate     = (int)(readU32BE(entry + 24) >> 16);
      if (rate <= 0 || channels <= 0) return;
      demuxer->audioSamples = buildSampleTable(source, stblStart, stblEnd, timescale, 0, &demuxer->audioSampleCount, 0);
      if (!demuxer->audioSamples) return;
      demuxer->hasAudio      = 1;
      demuxer->audioRate     = rate;
      demuxer->audioChannels = channels;
      logInfo("[demux-mp4] aac track, %d Hz, %d ch, %d samples\n", rate, channels, demuxer->audioSampleCount);
   }
}

int openMp4Demuxer(Mp4Demuxer *demuxer, const char *path)
{
   memset(demuxer, 0, sizeof *demuxer);
   if (openVideoSource(&demuxer->source, path) != 0) return -1;

   // section: find moov among the top-level boxes (it may trail the media data)
   uint64_t fileEnd = sizeVideoSource(&demuxer->source);
   uint64_t moovStart = 0, moovEnd = 0, pos = 0;
   int fragmented = 0;
   int guard = 0;
   while (pos + 8 <= fileEnd && guard++ < 256) {
      uint32_t type; uint64_t size, headerLen;
      if (readMp4Box(&demuxer->source, pos, &type, &size, &headerLen) != 0 || size == 0) break;
      if (type == FOURCC('m','o','o','v')) { moovStart = pos + headerLen; moovEnd = pos + size; }
      if (type == FOURCC('m','o','o','f')) fragmented = 1;
      pos += size;
   }
   if (!moovStart) { logError("[demux-mp4] no moov box found\n"); goto fail; }

   // section: mvhd (movie timescale + duration)
   uint64_t mvhdStart, mvhdEnd;
   if (findMp4ChildBox(&demuxer->source, moovStart, moovEnd, FOURCC('m','v','h','d'), &mvhdStart, &mvhdEnd) == 0) {
      uint8_t mvhd[32];
      if (readVideoSourceAt(&demuxer->source, mvhdStart, mvhd, sizeof mvhd) == 0) {
         if (mvhd[0] == 1) demuxer->durationNs = ticksToNs(readU64BE(mvhd + 24), readU32BE(mvhd + 20));
         else              demuxer->durationNs = ticksToNs(readU32BE(mvhd + 16), readU32BE(mvhd + 12));
      }
   }

   // section: every trak child of moov (video + audio live in separate traks)
   uint64_t trakScan = moovStart;
   int trakGuard = 0;
   while (trakScan + 8 <= moovEnd && trakGuard++ < 32) {
      uint64_t trakStart, trakEnd;
      if (findMp4ChildBox(&demuxer->source, trakScan, moovEnd, FOURCC('t','r','a','k'), &trakStart, &trakEnd) != 0) break;
      parseMp4Trak(demuxer, trakStart, trakEnd);
      trakScan = trakEnd;
   }
   if (!demuxer->h264.valid || !demuxer->videoSampleCount) {
      logError(fragmented ? "[demux-mp4] fragmented MP4 (moof) is not supported\n"
                          : "[demux-mp4] no decodable H.264 track\n");
      goto fail;
   }
   if (!demuxer->durationNs)
      demuxer->durationNs = demuxer->videoSamples[demuxer->videoSampleCount - 1].ptsNs + demuxer->frameDurationNs;

   // section: demux buffers + audio queue
   for (int i = 0; i < AU_BUFFER_COUNT; i++) {
      demuxer->auBuffers[i] = (uint8_t *)malloc(AU_BUFFER_SIZE);
      if (!demuxer->auBuffers[i]) goto fail;
   }
   demuxer->sampleBuffer = (uint8_t *)malloc(SAMPLE_BUFFER_SIZE);
   if (!demuxer->sampleBuffer) goto fail;
   demuxer->auCapacity     = AU_BUFFER_SIZE;
   demuxer->sampleCapacity = SAMPLE_BUFFER_SIZE;
   if (demuxer->hasAudio && createAudioAuQueue(&demuxer->audioQueue) != 0) demuxer->hasAudio = 0;   // no queue: play silent rather than fail
   return 0;

fail:
   closeMp4Demuxer(demuxer);
   return -1;
}

// ============================================================================
// read: next video sample -> Annex-B access unit, queueing the audio that falls due
// ============================================================================

// keeps the audio queue fed up to a lead ahead of the video position (audio samples sit near the
// matching video samples in the file, so these reads stay close to the video read position)
static void pumpAudioSamples(Mp4Demuxer *demuxer, uint64_t videoPts)
{
   while (demuxer->hasAudio && demuxer->audioCursor < demuxer->audioSampleCount && !isAudioAuQueueFull(&demuxer->audioQueue)) {
      const Mp4Sample *sample = &demuxer->audioSamples[demuxer->audioCursor];
      if (sample->ptsNs > videoPts + AUDIO_LEAD_NS) return;
      if (sample->size > AUDIO_AU_MAX_BYTES) {
         if (!demuxer->oversizeAudioWarned) { logWarn("[demux-mp4] oversized audio sample skipped\n"); demuxer->oversizeAudioWarned = 1; }
      } else if (readVideoSourceAt(&demuxer->source, sample->offset, demuxer->sampleBuffer, sample->size) == 0) {
         enqueueAudioAu(&demuxer->audioQueue, demuxer->sampleBuffer, (int)sample->size, sample->ptsNs);
      } else {   // read failure (truncated file): give up on audio; a seek resets the cursor
         logWarn("[demux-mp4] audio sample read failed, audio stops\n");
         demuxer->audioCursor = demuxer->audioSampleCount;
         return;
      }
      demuxer->audioCursor++;
   }
}

int readMp4VideoAu(Mp4Demuxer *demuxer, VideoAu *au)
{
   while (demuxer->videoCursor < demuxer->videoSampleCount) {
      const Mp4Sample *sample = &demuxer->videoSamples[demuxer->videoCursor];
      pumpAudioSamples(demuxer, sample->ptsNs);
      demuxer->videoCursor++;

      if (sample->size == 0 || sample->size > (uint32_t)demuxer->sampleCapacity) continue;   // corrupt entry: skip
      if (readVideoSourceAt(&demuxer->source, sample->offset, demuxer->sampleBuffer, sample->size) != 0) return 0;   // truncated: end of stream

      uint8_t *auBuffer = demuxer->auBuffers[demuxer->auIndex];   // alternate so the previous AU stays valid in the decoder
      if (!buildVideoAu(&demuxer->h264, demuxer->sampleBuffer, (int)sample->size, auBuffer, demuxer->auCapacity, sample->ptsNs, au)) continue;
      demuxer->auIndex = (demuxer->auIndex + 1) % AU_BUFFER_COUNT;
      return 1;
   }
   pumpAudioSamples(demuxer, demuxer->durationNs);   // video done: let the audio tail through
   return 0;
}

// ============================================================================
// seek
// ============================================================================

uint64_t seekMp4Demuxer(Mp4Demuxer *demuxer, uint64_t targetNs)
{
   clearAudioAuQueue(&demuxer->audioQueue);   // consumer is parked: discard queued audio

   // last keyframe presenting at or before the target (keyframe times increase monotonically even
   // though B-frame reordering makes the full pts sequence non-monotonic)
   int best = 0;
   for (int i = 0; i < demuxer->videoSampleCount; i++) {
      if (!demuxer->videoSamples[i].keyframe) continue;
      if (demuxer->videoSamples[i].ptsNs > targetNs) break;
      best = i;
   }
   demuxer->videoCursor = best;
   uint64_t landedNs = demuxer->videoSamples[best].ptsNs;

   // restart audio at the first sample due at the landed position
   demuxer->audioCursor = 0;
   while (demuxer->audioCursor < demuxer->audioSampleCount && demuxer->audioSamples[demuxer->audioCursor].ptsNs < landedNs)
      demuxer->audioCursor++;
   return landedNs;
}

void closeMp4Demuxer(Mp4Demuxer *demuxer)
{
   for (int i = 0; i < AU_BUFFER_COUNT; i++) { free(demuxer->auBuffers[i]); demuxer->auBuffers[i] = 0; }
   free(demuxer->sampleBuffer);
   free(demuxer->videoSamples);
   free(demuxer->audioSamples);
   demuxer->sampleBuffer = 0;
   demuxer->videoSamples = 0;
   demuxer->audioSamples = 0;
   destroyAudioAuQueue(&demuxer->audioQueue);
   closeVideoSource(&demuxer->source);
}
