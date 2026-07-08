// video-probe - inspects an MKV or MP4 container's track metadata (no full demux) and decides
// whether the v1 player can play it. See video-probe.h for the verdict/codec model.
//
// Two small parsers: an EBML walker for Matroska (MKV) and a box walker for ISOBMFF (MP4). Each
// finds the first video and audio track, classifies the codecs, and (for H.264) reads the profile
// to detect 10-bit. Everything reads through VideoSource, so it works on any VFS backend.
#include "video-probe.h"
#include "ebml.h"                // shared EBML element reader + Matroska IDs
#include "mp4.h"                 // shared ISOBMFF box reader
#include "h264.h"               // parseAvcc -> SPS max_num_ref_frames
#include "string-utilities.h"   // getExtension, strCmpICase
#include <string.h>
#include <stdio.h>              // snprintf (app/lib side; not a VSH PRX)

// only H.264 profiles the PS3 decodes are 8-bit; High 10 / High 4:2:2 / High 4:4:4 / CAVLC 4:4:4
// carry >8-bit samples. profile byte comes from the avcC / CodecPrivate record.
static int isHigh10Profile(int profile)
{
   return profile == 110 || profile == 122 || profile == 244 || profile == 44;
}

// reads what the verdict needs out of an avcC: the bit depth (from the profile byte) and whether the
// SPS's reference-frame count fits the PS3 decoder's frame pool (the DPB).
static void inspectAvcc(const uint8_t *avcc, int size, int *bitDepth, int *maxRefFrames, int *refFramesExceedDpb)
{
   if (size >= 2) *bitDepth = isHigh10Profile(avcc[1]) ? 10 : 8;
   H264Config config;
   if (parseAvcc(avcc, size, &config) == 0) {
      *maxRefFrames       = config.maxRefFrames;
      *refFramesExceedDpb = config.refFramesExceedDpb;
   }
}

// ============================================================================
// codec classification (shared by both containers)
// ============================================================================

static VideoCodec classifyMkvVideo(const char *codecId, char *nameOut, int nameCap)
{
   if (strncmp(codecId, "V_MPEG4/ISO/AVC", 15) == 0) { strCopy(nameOut, nameCap, "H.264");          return VIDEO_CODEC_H264;  }
   if (strncmp(codecId, "V_MPEGH/ISO/HEVC", 16) == 0){ strCopy(nameOut, nameCap, "HEVC (H.265)");   return VIDEO_CODEC_HEVC;  }
   if (strncmp(codecId, "V_MPEG4", 7) == 0)          { strCopy(nameOut, nameCap, "MPEG-4 (DivX)");  return VIDEO_CODEC_MPEG4; }
   if (strncmp(codecId, "V_MPEG1", 7) == 0 ||
       strncmp(codecId, "V_MPEG2", 7) == 0)          { strCopy(nameOut, nameCap, "MPEG-2");         return VIDEO_CODEC_MPEG2; }
   strCopy(nameOut, nameCap, codecId);
   return VIDEO_CODEC_OTHER;
}

static VideoAudioCodec classifyMkvAudio(const char *codecId, char *nameOut, int nameCap)
{
   if (strncmp(codecId, "A_AAC", 5) == 0)      { strCopy(nameOut, nameCap, "AAC"); return AUDIO_CODEC_AAC; }
   if (strncmp(codecId, "A_AC3", 5) == 0)      { strCopy(nameOut, nameCap, "AC3"); return AUDIO_CODEC_AC3; }
   if (strncmp(codecId, "A_MPEG/L3", 9) == 0)  { strCopy(nameOut, nameCap, "MP3"); return AUDIO_CODEC_MP3; }
   strCopy(nameOut, nameCap, codecId);
   return AUDIO_CODEC_OTHER;
}

// ============================================================================
// EBML / Matroska
// ============================================================================

#define MKV_SCAN_CAP  (16u * 1024 * 1024)   // stop hunting for Tracks after this much of the segment

// parses one TrackEntry's children in [start, end); records into the playability struct if this is
// the first video or audio track seen.
static void parseMkvTrackEntry(VideoSource *source, uint64_t start, uint64_t end, VideoPlayability *out)
{
   int      trackType = 0;                 // 1 = video, 2 = audio
   char     codecId[48] = {0};
   int      width = 0, height = 0;
   int      bitDepth = 0, maxRefFrames = 0, refFramesExceedDpb = 0;

   uint64_t pos = start;
   int guard = 0;
   while (pos < end && guard++ < 64) {
      if (seekVideoSource(source, pos) != 0) return;
      uint32_t id; uint64_t size; int unknown = 0;
      if (readEbmlElement(source, &id, &size, &unknown) != 0 || unknown) return;
      uint64_t dataStart = getVideoSourcePosition(source);

      if (id == EBML_ID_TRACKTYPE) {
         uint8_t b = 0; readVideoSource(source, &b, 1); trackType = b;
      } else if (id == EBML_ID_CODECID) {
         uint64_t n = size < sizeof codecId - 1 ? size : sizeof codecId - 1;
         readVideoSource(source, codecId, n); codecId[n] = 0;
      } else if (id == EBML_ID_CODECPRIVATE) {
         uint8_t avcc[256];                 // avcC record (SPS/PPS); enough to reach the SPS
         int n = size < sizeof avcc ? (int)size : (int)sizeof avcc;
         if (n >= 2 && readVideoSource(source, avcc, n) == n) inspectAvcc(avcc, n, &bitDepth, &maxRefFrames, &refFramesExceedDpb);
      } else if (id == EBML_ID_VIDEO) {
         uint64_t vpos = dataStart, vend = dataStart + size;
         int vguard = 0;
         while (vpos < vend && vguard++ < 32) {
            if (seekVideoSource(source, vpos) != 0) break;
            uint32_t vid; uint64_t vsize; int vunknown = 0;
            if (readEbmlElement(source, &vid, &vsize, &vunknown) != 0 || vunknown) break;
            uint64_t vdata = getVideoSourcePosition(source);
            if (vid == EBML_ID_PIXELWIDTH || vid == EBML_ID_PIXELHEIGHT) {
               uint64_t v = 0;
               for (uint64_t i = 0; i < vsize && i < 8; i++) { uint8_t c = 0; readVideoSource(source, &c, 1); v = (v << 8) | c; }
               if (vid == EBML_ID_PIXELWIDTH) width = (int)v; else height = (int)v;
            }
            vpos = vdata + vsize;
         }
      }
      pos = dataStart + size;
   }

   if (trackType == 1 && out->videoCodec == VIDEO_CODEC_NONE) {
      out->videoCodec = classifyMkvVideo(codecId, out->videoCodecName, sizeof out->videoCodecName);
      out->width = width; out->height = height;
      out->bitDepth = bitDepth;
      out->maxRefFrames = maxRefFrames;
      out->refFramesExceedDpb = refFramesExceedDpb;
   } else if (trackType == 2 && out->audioCodec == AUDIO_CODEC_NONE) {
      out->audioCodec = classifyMkvAudio(codecId, out->audioCodecName, sizeof out->audioCodecName);
   }
}

// walks the Tracks element in [start, end), parsing each TrackEntry.
static void parseMkvTracks(VideoSource *source, uint64_t start, uint64_t end, VideoPlayability *out)
{
   uint64_t pos = start;
   int guard = 0;
   while (pos < end && guard++ < 64) {
      if (seekVideoSource(source, pos) != 0) return;
      uint32_t id; uint64_t size; int unknown = 0;
      if (readEbmlElement(source, &id, &size, &unknown) != 0 || unknown) return;
      uint64_t dataStart = getVideoSourcePosition(source);
      if (id == EBML_ID_TRACKENTRY) parseMkvTrackEntry(source, dataStart, dataStart + size, out);
      pos = dataStart + size;
   }
}

// returns 0 if the container was recognised as MKV and walked, -1 otherwise.
static int probeMkv(VideoSource *source, VideoPlayability *out)
{
   // EBML header first
   if (seekVideoSource(source, 0) != 0) return -1;
   uint32_t id; uint64_t size; int unknown = 0;
   if (readEbmlElement(source, &id, &size, &unknown) != 0 || id != EBML_ID_HEADER) return -1;
   uint64_t segScan = getVideoSourcePosition(source) + size;   // skip the header body

   // Segment
   if (seekVideoSource(source, segScan) != 0) return -1;
   if (readEbmlElement(source, &id, &size, &unknown) != 0 || id != EBML_ID_SEGMENT) return -1;
   uint64_t segStart = getVideoSourcePosition(source);
   uint64_t segEnd   = unknown ? segStart + MKV_SCAN_CAP : segStart + size;

   // walk the segment's children until Tracks is found (it precedes the clusters)
   uint64_t pos = segStart;
   int guard = 0;
   while (pos < segEnd && guard++ < 256) {
      if (seekVideoSource(source, pos) != 0) break;
      if (readEbmlElement(source, &id, &size, &unknown) != 0) break;
      uint64_t dataStart = getVideoSourcePosition(source);
      if (id == EBML_ID_TRACKS) { parseMkvTracks(source, dataStart, dataStart + size, out); return 0; }
      if (id == EBML_ID_CLUSTER) break;   // media started; no Tracks up front
      if (unknown) break;
      pos = dataStart + size;
   }
   return 0;   // recognised as MKV even if Tracks wasn't located (verdict handles empty codecs)
}

// ============================================================================
// ISOBMFF / MP4
// ============================================================================

static VideoCodec classifyMp4Video(uint32_t fourcc, char *nameOut, int nameCap)
{
   if (fourcc == FOURCC('a','v','c','1') || fourcc == FOURCC('a','v','c','3')) { strCopy(nameOut, nameCap, "H.264");         return VIDEO_CODEC_H264;  }
   if (fourcc == FOURCC('h','e','v','1') || fourcc == FOURCC('h','v','c','1')) { strCopy(nameOut, nameCap, "HEVC (H.265)");  return VIDEO_CODEC_HEVC;  }
   if (fourcc == FOURCC('m','p','4','v'))                                      { strCopy(nameOut, nameCap, "MPEG-4 (DivX)"); return VIDEO_CODEC_MPEG4; }
   char raw[5]; fourccToStr(fourcc, raw);
   strCopy(nameOut, nameCap, raw);
   return VIDEO_CODEC_OTHER;
}

static VideoAudioCodec classifyMp4Audio(uint32_t fourcc, char *nameOut, int nameCap)
{
   if (fourcc == FOURCC('m','p','4','a')) { strCopy(nameOut, nameCap, "AAC"); return AUDIO_CODEC_AAC; }
   if (fourcc == FOURCC('a','c','-','3')) { strCopy(nameOut, nameCap, "AC3"); return AUDIO_CODEC_AC3; }
   char raw[5]; fourccToStr(fourcc, raw);
   strCopy(nameOut, nameCap, raw);
   return AUDIO_CODEC_OTHER;
}

// parses a stsd payload (fullbox: 4 version/flags + 4 entry_count, then sample entries) and records
// the codec of its first sample entry into the struct. `handler` is the trak's hdlr type — it, not
// the entry fourcc, says whether the track is video or audio (unknown codecs stay classifiable).
static void parseMp4Stsd(VideoSource *source, uint64_t start, uint64_t end, uint32_t handler, VideoPlayability *out)
{
   uint64_t entryPos = start + 8;   // skip version/flags + entry_count
   if (entryPos + 8 > end) return;

   uint32_t type; uint64_t size, headerLen;
   if (readMp4Box(source, entryPos, &type, &size, &headerLen) != 0) return;
   uint64_t entryEnd = entryPos + size;
   if (entryEnd > end) entryEnd = end;

   // video sample entries carry width/height at payload offset 24/26 (after 6 reserved + 2 dri +
   // 16 predefined); their child boxes (avcC, ...) start after the 78-byte VisualSampleEntry body.
   if (handler == FOURCC('v','i','d','e')) {
      if (out->videoCodec != VIDEO_CODEC_NONE) return;
      out->videoCodec = classifyMp4Video(type, out->videoCodecName, sizeof out->videoCodecName);

      uint8_t wh[4];
      if (readVideoSourceAt(source, entryPos + 8 + 24, wh, 4) == 0) {
         out->width  = readU16BE(wh);
         out->height = readU16BE(wh + 2);
      }
      if (out->videoCodec == VIDEO_CODEC_H264) {
         uint64_t cfgStart, cfgEnd;
         if (findMp4ChildBox(source, entryPos + 8 + 78, entryEnd, FOURCC('a','v','c','C'), &cfgStart, &cfgEnd) == 0) {
            uint8_t avcc[256];   // avcC record (SPS/PPS)
            int n = (int)(cfgEnd - cfgStart);
            if (n > (int)sizeof avcc) n = sizeof avcc;
            if (n >= 2 && readVideoSourceAt(source, cfgStart, avcc, n) == 0)
               inspectAvcc(avcc, n, &out->bitDepth, &out->maxRefFrames, &out->refFramesExceedDpb);
         }
      }
   } else if (handler == FOURCC('s','o','u','n')) {
      if (out->audioCodec != AUDIO_CODEC_NONE) return;
      out->audioCodec = classifyMp4Audio(type, out->audioCodecName, sizeof out->audioCodecName);
   }
}

// descends one trak: mdia -> (hdlr for the track type) -> minf -> stbl -> stsd.
static void parseMp4Trak(VideoSource *source, uint64_t start, uint64_t end, VideoPlayability *out)
{
   uint64_t mdiaStart, mdiaEnd;
   if (findMp4ChildBox(source, start, end, FOURCC('m','d','i','a'), &mdiaStart, &mdiaEnd) != 0) return;

   uint64_t hdlrStart, hdlrEnd;
   if (findMp4ChildBox(source, mdiaStart, mdiaEnd, FOURCC('h','d','l','r'), &hdlrStart, &hdlrEnd) != 0) return;
   uint8_t hdlr[12];
   if (readVideoSourceAt(source, hdlrStart, hdlr, sizeof hdlr) != 0) return;
   uint32_t handler = readU32BE(hdlr + 8);

   uint64_t s = mdiaStart, e = mdiaEnd;
   if (findMp4ChildBox(source, s, e, FOURCC('m','i','n','f'), &s, &e) != 0) return;
   if (findMp4ChildBox(source, s, e, FOURCC('s','t','b','l'), &s, &e) != 0) return;
   if (findMp4ChildBox(source, s, e, FOURCC('s','t','s','d'), &s, &e) != 0) return;
   parseMp4Stsd(source, s, e, handler, out);
}

// returns 0 if the container was recognised as MP4 and walked, -1 otherwise.
static int probeMp4(VideoSource *source, VideoPlayability *out)
{
   // first top-level box should be ftyp
   uint32_t type; uint64_t size, headerLen;
   if (readMp4Box(source, 0, &type, &size, &headerLen) != 0 || type != FOURCC('f','t','y','p')) return -1;

   // scan top-level boxes for moov (seeking past mdat handles moov-at-end files)
   uint64_t fileEnd = getVideoSourceSize(source);
   uint64_t pos = 0;
   int guard = 0;
   while (pos + 8 <= fileEnd && guard++ < 256) {
      if (readMp4Box(source, pos, &type, &size, &headerLen) != 0 || size == 0) break;
      if (type == FOURCC('m','o','o','v')) {
         uint64_t moovStart = pos + headerLen, moovEnd = pos + size;
         uint64_t trakStart = moovStart;
         int trakGuard = 0;
         // iterate every trak child of moov (video + audio live in separate traks)
         while (trakStart + 8 <= moovEnd && trakGuard++ < 32) {
            uint64_t trakPayloadStart, trakPayloadEnd;
            if (findMp4ChildBox(source, trakStart, moovEnd, FOURCC('t','r','a','k'), &trakPayloadStart, &trakPayloadEnd) != 0) break;
            parseMp4Trak(source, trakPayloadStart, trakPayloadEnd, out);
            trakStart = trakPayloadEnd;   // continue after this trak
         }
         return 0;
      }
      pos += size;
   }
   return 0;   // recognised as MP4 even if moov wasn't located
}

// ============================================================================
// verdict
// ============================================================================

static void buildUnsupportedReason(VideoPlayability *out)
{
   char parts[2][80];
   int n = 0;

   if (out->videoCodec == VIDEO_CODEC_H264 && out->bitDepth == 10)
      strCopy(parts[n++], sizeof parts[0], "10-bit H.264 video");
   else if (out->videoCodec == VIDEO_CODEC_H264 && out->refFramesExceedDpb)
      strCopy(parts[n++], sizeof parts[0], "H.264 video with more reference frames than the PS3 can decode");
   else if (out->videoCodec != VIDEO_CODEC_H264)
      snprintf(parts[n++], sizeof parts[0], "%s video",
               out->videoCodecName[0] ? out->videoCodecName : "an unrecognised");

   if (out->audioCodec == AUDIO_CODEC_OTHER)
      snprintf(parts[n++], sizeof parts[0], "%s audio",
               out->audioCodecName[0] ? out->audioCodecName : "unsupported");

   if (n == 0)
      strCopy(out->reason, sizeof out->reason, "This video isn't in a supported format.");
   else if (n == 1)
      snprintf(out->reason, sizeof out->reason, "This video contains %s, which isn't supported.", parts[0]);
   else
      snprintf(out->reason, sizeof out->reason, "This video contains %s and %s, which aren't supported.", parts[0], parts[1]);
}

int isVideoFile(const char *name)
{
   const char *ext = getExtension(name);
   if (!ext) return 0;
   static const char *videoExts[] = { "mp4","mkv","avi","mov","wmv","flv","webm","m4v", 0 };
   for (int i = 0; videoExts[i]; i++)
      if (strCmpICase(ext, videoExts[i]) == 0) return 1;
   return 0;
}

VideoVerdict probeVideo(const char *path, VideoPlayability *out)
{
   memset(out, 0, sizeof *out);

   VideoSource source;
   if (openVideoSource(&source, path) != 0) {
      out->verdict = VIDEO_UNREADABLE;
      strCopy(out->reason, sizeof out->reason, "This file couldn't be opened.");
      return out->verdict;
   }

   uint8_t magic[12] = {0};
   int readMagic = (readVideoSourceAt(&source, 0, magic, sizeof magic) == 0);
   int recognised = -1;
   if (readMagic) {
      if (magic[0] == 0x1A && magic[1] == 0x45 && magic[2] == 0xDF && magic[3] == 0xA3)
         recognised = probeMkv(&source, out);
      else if (magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p')
         recognised = probeMp4(&source, out);
   }
   closeVideoSource(&source);

   if (recognised != 0) {
      out->verdict = VIDEO_UNREADABLE;
      strCopy(out->reason, sizeof out->reason, "This file isn't a readable MKV or MP4 video.");
      return out->verdict;
   }

   // v1 plays 8-bit H.264 video whose reference frames fit the decoder (audio is decoded separately;
   // an unsupported audio track just plays silent, so it doesn't block playback on its own).
   if (out->videoCodec == VIDEO_CODEC_H264 && out->bitDepth != 10 && !out->refFramesExceedDpb) {
      out->verdict = VIDEO_PLAYABLE;
      out->reason[0] = 0;
   } else {
      out->verdict = VIDEO_UNSUPPORTED;
      buildUnsupportedReason(out);
   }
   return out->verdict;
}
