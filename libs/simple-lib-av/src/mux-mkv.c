// mux-mkv - streaming Matroska writer (see mux-mkv.h).

#include "mux-mkv.h"
#include "ebml.h"        // segment-level element ids
#include "dbg.h"
#include <string.h>
#include <stdlib.h>

// EBML-header element ids (ebml.h only collects the segment-level ones the reader needs).
#define ID_EBML_VERSION       0x4286u
#define ID_EBML_READVERSION   0x42F7u
#define ID_EBML_MAXIDLENGTH   0x42F2u
#define ID_EBML_MAXSIZELENGTH 0x42F3u
#define ID_DOCTYPE            0x4282u
#define ID_DOCTYPEVERSION     0x4287u
#define ID_DOCTYPEREADVERSION 0x4285u
#define ID_TRACKUID           0x73C5u
#define ID_MUXINGAPP          0x4D80u
#define ID_WRITINGAPP         0x5741u

#define VIDEO_TRACK    1
#define AUDIO_TRACK    2
#define TIMECODE_MS_NS 1000000ull   // one MKV timecode tick = 1 ms

// keep clusters short so a SimpleBlock's 16-bit relative timecode never overflows and seeking stays cheap:
// start a fresh cluster on a keyframe once the current one spans at least MIN, and force one at MAX.
#define CLUSTER_MIN_MS 1000
#define CLUSTER_MAX_MS 30000

// ---- primitive encoders ----

// EBML element id, written as its minimal (canonical) byte width.
static int encodeId(uint32_t id, uint8_t *out)
{
   int length = id >= 0x01000000u ? 4 : id >= 0x00010000u ? 3 : id >= 0x00000100u ? 2 : 1;
   for (int i = 0; i < length; i++) out[i] = (uint8_t)(id >> (8 * (length - 1 - i)));
   return length;
}

// EBML size as a variable-length integer (all-ones is reserved for "unknown", so a length holds 2^(7L)-1 values).
static int encodeVintSize(uint64_t value, uint8_t *out)
{
   int length = 1;
   while (length < 8 && value >= (1ull << (7 * length)) - 1) length++;
   uint64_t marked = value | (1ull << (7 * length));
   for (int i = 0; i < length; i++) out[i] = (uint8_t)(marked >> (8 * (length - 1 - i)));
   return length;
}

// small append helpers onto a caller-sized stack buffer (buffers here are generously sized; no bounds checks)
static void putId(uint8_t *b, int *n, uint32_t id)                  { *n += encodeId(id, b + *n); }
static void putSize(uint8_t *b, int *n, uint64_t size)             { *n += encodeVintSize(size, b + *n); }

static void putElem(uint8_t *b, int *n, uint32_t id, const void *data, int size)
{
   putId(b, n, id); putSize(b, n, size);
   memcpy(b + *n, data, size); *n += size;
}

static void putStr(uint8_t *b, int *n, uint32_t id, const char *text) { putElem(b, n, id, text, (int)strlen(text)); }

// unsigned integer element, big-endian and trimmed to its significant bytes (at least one).
static void putUint(uint8_t *b, int *n, uint32_t id, uint64_t value)
{
   uint8_t raw[8];
   for (int i = 0; i < 8; i++) raw[i] = (uint8_t)(value >> (8 * (7 - i)));
   int start = 0;
   while (start < 7 && raw[start] == 0) start++;
   putElem(b, n, id, raw + start, 8 - start);
}

// 64-bit IEEE float element (MKV Duration / SamplingFrequency).
static void putFloat(uint8_t *b, int *n, uint32_t id, double value)
{
   uint64_t bits;
   memcpy(&bits, &value, sizeof bits);
   uint8_t raw[8];
   for (int i = 0; i < 8; i++) raw[i] = (uint8_t)(bits >> (8 * (7 - i)));
   putElem(b, n, id, raw, 8);
}

// ---- H.264 helpers: Annex-B walking + avcC ----

static int isStartCode(const uint8_t *d, int size, int i, int *codeLen)
{
   if (i + 4 <= size && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) { *codeLen = 4; return 1; }
   if (i + 3 <= size && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)                  { *codeLen = 3; return 1; }
   return 0;
}

// total size of the access unit once each Annex-B NAL is rewritten as a 4-byte-length-prefixed NAL. -1 if empty.
static int lengthPrefixedSize(const uint8_t *d, int size)
{
   int i = 0, code;
   while (i < size && !isStartCode(d, size, i, &code)) i++;
   if (i >= size) return -1;
   i += code;
   int nalStart = i, total = 0;
   while (i < size) {
      if (isStartCode(d, size, i, &code)) { total += 4 + (i - nalStart); i += code; nalStart = i; }
      else i++;
   }
   if (size > nalStart) total += 4 + (size - nalStart);
   return total;
}

static int emitNal(MkvMuxer *mux, const uint8_t *nal, int length);   // fwd (defined after clusterPut)

// rewrite each Annex-B NAL of the access unit as [4-byte length][nal] into the current cluster. 0 / -1.
static int lengthPrefixedToCluster(MkvMuxer *mux, const uint8_t *d, int size)
{
   int i = 0, code;
   while (i < size && !isStartCode(d, size, i, &code)) i++;
   if (i >= size) return -1;
   i += code;
   int nalStart = i;
   while (i < size) {
      if (isStartCode(d, size, i, &code)) { if (emitNal(mux, d + nalStart, i - nalStart)) return -1; i += code; nalStart = i; }
      else i++;
   }
   return (size > nalStart) ? emitNal(mux, d + nalStart, size - nalStart) : 0;
}

// build an avcC (AVCDecoderConfigurationRecord) from the demuxer's Annex-B SPS/PPS header. -1 if malformed.
static int buildAvcc(const H264Config *h264, uint8_t *out, int cap)
{
   const uint8_t *sps = NULL, *pps = NULL;
   int spsLen = 0, ppsLen = 0;
   const uint8_t *d = h264->header;
   int size = h264->headerSize, i = 0, code;

   while (i < size && !isStartCode(d, size, i, &code)) i++;
   if (i >= size) return -1;
   i += code;
   int nalStart = i;
   for (;;) {
      int atEnd = i >= size;
      if (atEnd || isStartCode(d, size, i, &code)) {
         const uint8_t *nal = d + nalStart;
         int nalLen = (atEnd ? size : i) - nalStart;
         if (nalLen > 0) {
            int type = nal[0] & 0x1F;
            if (type == 7 && !sps) { sps = nal; spsLen = nalLen; }
            else if (type == 8 && !pps) { pps = nal; ppsLen = nalLen; }
         }
         if (atEnd) break;
         i += code; nalStart = i;
      } else i++;
   }
   if (!sps || !pps || spsLen < 4 || 11 + spsLen + ppsLen > cap) return -1;

   int n = 0;
   out[n++] = 1;                        // configurationVersion
   out[n++] = sps[1];                   // AVCProfileIndication
   out[n++] = sps[2];                   // profile_compatibility
   out[n++] = sps[3];                   // AVCLevelIndication
   out[n++] = 0xFF;                     // 111111 + lengthSizeMinusOne (3 -> 4-byte lengths)
   out[n++] = 0xE1;                     // 111 + numOfSequenceParameterSets (1)
   out[n++] = (uint8_t)(spsLen >> 8); out[n++] = (uint8_t)spsLen;
   memcpy(out + n, sps, spsLen); n += spsLen;
   out[n++] = 1;                        // numOfPictureParameterSets
   out[n++] = (uint8_t)(ppsLen >> 8); out[n++] = (uint8_t)ppsLen;
   memcpy(out + n, pps, ppsLen); n += ppsLen;
   return n;
}

// AAC AudioSpecificConfig (2 bytes): object type 2 (AAC-LC), sampling-frequency index, channel config.
static void buildAsc(int rate, int channels, uint8_t asc[2])
{
   static const int table[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
   int index = 4;   // default 44100
   for (int i = 0; i < 13; i++) if (table[i] == rate) { index = i; break; }
   asc[0] = (uint8_t)((2 << 3) | ((index >> 1) & 0x07));
   asc[1] = (uint8_t)(((index & 1) << 7) | ((channels & 0x0F) << 3));
}

// ---- cluster buffer ----

static int clusterPut(MkvMuxer *mux, const void *data, int length)
{
   if (mux->clusterLen + length > mux->clusterCap) {
      int cap = mux->clusterCap ? mux->clusterCap : 65536;
      while (cap < mux->clusterLen + length) cap *= 2;
      uint8_t *grown = (uint8_t *)realloc(mux->cluster, cap);
      if (!grown) return -1;
      mux->cluster = grown; mux->clusterCap = cap;
   }
   memcpy(mux->cluster + mux->clusterLen, data, length);
   mux->clusterLen += length;
   return 0;
}

static int emitNal(MkvMuxer *mux, const uint8_t *nal, int length)
{
   uint8_t prefix[4] = { (uint8_t)(length >> 24), (uint8_t)(length >> 16), (uint8_t)(length >> 8), (uint8_t)length };
   if (clusterPut(mux, prefix, 4)) return -1;
   return clusterPut(mux, nal, length);
}

static int flushCluster(MkvMuxer *mux)
{
   if (mux->clusterBaseMs == UINT64_MAX || mux->clusterLen == 0) return 0;
   uint8_t header[16];
   int hn = 0;
   putId(header, &hn, EBML_ID_CLUSTER);
   putSize(header, &hn, mux->clusterLen);
   if (writeFs(&mux->file, header, hn) != hn) return -1;
   if (writeFs(&mux->file, mux->cluster, mux->clusterLen) != mux->clusterLen) return -1;
   mux->bytesWritten += hn + mux->clusterLen;
   mux->clusterLen = 0;
   mux->clusterBaseMs = UINT64_MAX;
   return 0;
}

static int openCluster(MkvMuxer *mux, uint64_t baseMs)
{
   mux->clusterBaseMs = baseMs;
   mux->clusterLen = 0;
   uint8_t timecode[16];
   int tn = 0;
   putUint(timecode, &tn, EBML_ID_TIMECODE, baseMs);   // first child of the cluster
   return clusterPut(mux, timecode, tn);
}

// ensure a cluster is open that this block fits in: keep clusters short, and prefer to break on keyframes.
static int prepareCluster(MkvMuxer *mux, uint64_t ptsMs, int isVideoKeyframe)
{
   if (mux->clusterBaseMs == UINT64_MAX) return openCluster(mux, ptsMs);
   long relative = (long)ptsMs - (long)mux->clusterBaseMs;
   if ((isVideoKeyframe && relative >= CLUSTER_MIN_MS) || relative >= CLUSTER_MAX_MS) {
      if (flushCluster(mux)) return -1;
      return openCluster(mux, ptsMs);
   }
   return 0;
}

// write a SimpleBlock header (id + size + track + relative timecode + flags) into the cluster.
static int writeBlockHeader(MkvMuxer *mux, int track, uint64_t ptsMs, int keyframe, int payloadLen)
{
   int relative = (int)((long)ptsMs - (long)mux->clusterBaseMs);
   if (relative < 0) relative = 0;
   uint8_t block[16];
   int bn = 0;
   putId(block, &bn, EBML_ID_SIMPLEBLOCK);
   putSize(block, &bn, 1 + 2 + 1 + payloadLen);   // track vint + int16 timecode + flags + frame
   block[bn++] = (uint8_t)(0x80 | track);         // track number as a 1-byte EBML vint
   block[bn++] = (uint8_t)((relative >> 8) & 0xFF);
   block[bn++] = (uint8_t)(relative & 0xFF);
   block[bn++] = keyframe ? 0x80 : 0x00;          // flags: keyframe bit
   return clusterPut(mux, block, bn);
}

// ---- structural headers ----

static int writeChunk(MkvMuxer *mux, const uint8_t *data, int length)
{
   if (writeFs(&mux->file, data, length) != length) return -1;
   mux->bytesWritten += length;
   return 0;
}

static int writeEbmlHeader(MkvMuxer *mux)
{
   uint8_t body[64];
   int n = 0;
   putUint(body, &n, ID_EBML_VERSION, 1);
   putUint(body, &n, ID_EBML_READVERSION, 1);
   putUint(body, &n, ID_EBML_MAXIDLENGTH, 4);
   putUint(body, &n, ID_EBML_MAXSIZELENGTH, 8);
   putStr (body, &n, ID_DOCTYPE, "matroska");
   putUint(body, &n, ID_DOCTYPEVERSION, 2);
   putUint(body, &n, ID_DOCTYPEREADVERSION, 2);
   uint8_t header[16];
   int hn = 0;
   putId(header, &hn, EBML_ID_HEADER);
   putSize(header, &hn, n);
   if (writeChunk(mux, header, hn)) return -1;
   return writeChunk(mux, body, n);
}

static int writeSegmentHeader(MkvMuxer *mux)
{
   uint8_t out[16];
   int n = 0;
   putId(out, &n, EBML_ID_SEGMENT);
   out[n++] = 0x01;                          // 8-byte "unknown size": segment runs to end of file
   for (int i = 0; i < 7; i++) out[n++] = 0xFF;
   return writeChunk(mux, out, n);
}

static int writeInfo(MkvMuxer *mux, uint64_t durationNs)
{
   uint8_t body[128];
   int n = 0;
   putUint(body, &n, EBML_ID_TIMECODESCALE, TIMECODE_MS_NS);
   if (durationNs) putFloat(body, &n, EBML_ID_DURATION, (double)durationNs / (double)TIMECODE_MS_NS);
   putStr(body, &n, ID_MUXINGAPP, "yo-player");
   putStr(body, &n, ID_WRITINGAPP, "yo-player");
   uint8_t header[16];
   int hn = 0;
   putId(header, &hn, EBML_ID_INFO);
   putSize(header, &hn, n);
   if (writeChunk(mux, header, hn)) return -1;
   return writeChunk(mux, body, n);
}

static int writeTracks(MkvMuxer *mux, const H264Config *h264, int width, int height,
                       uint64_t frameDurationNs, int audioRate, int audioChannels)
{
   uint8_t avcc[640];
   int avccLen = buildAvcc(h264, avcc, sizeof avcc);
   if (avccLen < 0) { logError("[mkv] avcC build failed\n"); return -1; }

   uint8_t tracks[2560];
   int tn = 0;

   // video track entry
   uint8_t video[1280];
   int vn = 0;
   putUint(video, &vn, EBML_ID_TRACKNUMBER, VIDEO_TRACK);
   putUint(video, &vn, ID_TRACKUID, VIDEO_TRACK);
   putUint(video, &vn, EBML_ID_TRACKTYPE, 1);
   putStr (video, &vn, EBML_ID_CODECID, "V_MPEG4/ISO/AVC");
   if (frameDurationNs) putUint(video, &vn, EBML_ID_DEFAULTDURATION, frameDurationNs);
   putElem(video, &vn, EBML_ID_CODECPRIVATE, avcc, avccLen);
   {
      uint8_t settings[32];
      int sn = 0;
      putUint(settings, &sn, EBML_ID_PIXELWIDTH, width);
      putUint(settings, &sn, EBML_ID_PIXELHEIGHT, height);
      putElem(video, &vn, EBML_ID_VIDEO, settings, sn);
   }
   putElem(tracks, &tn, EBML_ID_TRACKENTRY, video, vn);

   // audio track entry (optional)
   if (audioRate > 0 && audioChannels > 0) {
      uint8_t asc[2];
      buildAsc(audioRate, audioChannels, asc);
      uint8_t audio[128];
      int an = 0;
      putUint(audio, &an, EBML_ID_TRACKNUMBER, AUDIO_TRACK);
      putUint(audio, &an, ID_TRACKUID, AUDIO_TRACK);
      putUint(audio, &an, EBML_ID_TRACKTYPE, 2);
      putStr (audio, &an, EBML_ID_CODECID, "A_AAC");
      putElem(audio, &an, EBML_ID_CODECPRIVATE, asc, 2);
      {
         uint8_t settings[32];
         int sn = 0;
         putFloat(settings, &sn, EBML_ID_SAMPLINGFREQ, (double)audioRate);
         putUint (settings, &sn, EBML_ID_CHANNELS, audioChannels);
         putElem(audio, &an, EBML_ID_AUDIO, settings, sn);
      }
      putElem(tracks, &tn, EBML_ID_TRACKENTRY, audio, an);
   }

   uint8_t header[16];
   int hn = 0;
   putId(header, &hn, EBML_ID_TRACKS);
   putSize(header, &hn, tn);
   if (writeChunk(mux, header, hn)) return -1;
   return writeChunk(mux, tracks, tn);
}

// ---- public interface ----

int openMkvMuxer(MkvMuxer *mux, const char *path, const H264Config *h264, int width, int height,
                 uint64_t frameDurationNs, uint64_t durationNs, int audioRate, int audioChannels)
{
   memset(mux, 0, sizeof *mux);
   mux->clusterBaseMs = UINT64_MAX;
   mux->hasAudio = (audioRate > 0 && audioChannels > 0);

   if (openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &mux->file) != 0) { logError("[mkv] open '%s' failed\n", path); return -1; }
   mux->isOpen = 1;

   if (writeEbmlHeader(mux) || writeSegmentHeader(mux) || writeInfo(mux, durationNs) ||
       writeTracks(mux, h264, width, height, frameDurationNs, audioRate, audioChannels)) {
      closeFs(&mux->file);
      mux->isOpen = 0;
      deleteFile(path);
      return -1;
   }
   return 0;
}

int writeMkvVideo(MkvMuxer *mux, const uint8_t *annexB, int size, uint64_t ptsNs, int keyframe)
{
   uint64_t ptsMs = ptsNs / TIMECODE_MS_NS;
   int payloadLen = lengthPrefixedSize(annexB, size);
   if (payloadLen < 0) return -1;
   if (prepareCluster(mux, ptsMs, keyframe)) return -1;
   if (writeBlockHeader(mux, VIDEO_TRACK, ptsMs, keyframe, payloadLen)) return -1;
   return lengthPrefixedToCluster(mux, annexB, size);
}

int writeMkvAudio(MkvMuxer *mux, const uint8_t *aac, int size, uint64_t ptsNs)
{
   if (size <= 0) return 0;
   uint64_t ptsMs = ptsNs / TIMECODE_MS_NS;
   if (prepareCluster(mux, ptsMs, 0)) return -1;
   if (writeBlockHeader(mux, AUDIO_TRACK, ptsMs, 1, size)) return -1;   // every AAC frame is a random-access point
   return clusterPut(mux, aac, size);
}

int closeMkvMuxer(MkvMuxer *mux)
{
   int rc = 0;
   if (mux->isOpen) {
      if (flushCluster(mux)) rc = -1;
      if (closeFs(&mux->file) != 0) rc = -1;
      mux->isOpen = 0;
   }
   free(mux->cluster);
   mux->cluster = NULL;
   mux->clusterCap = mux->clusterLen = 0;
   return rc;
}
