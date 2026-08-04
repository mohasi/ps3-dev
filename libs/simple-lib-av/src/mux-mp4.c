// mux-mp4 - MP4 (ISO base media) writer (see mux-mp4.h).

#include "mux-mp4.h"
#include "dbg.h"
#include <string.h>
#include <stdlib.h>

#define VIDEO_TIMESCALE 90000    // ticks per second for the video track
#define MOVIE_TIMESCALE 1000     // ticks per second for the movie header
#define AAC_FRAME_SAMPLES 1024   // audio samples per AAC frame; the audio track's per-frame duration
#define NS_PER_SECOND 1000000000ull

static uint32_t nsToTicks(uint64_t ns, uint32_t timescale) { return (uint32_t)(ns * timescale / NS_PER_SECOND); }

// ---- H.264 and AAC: what the container needs, built from what the demuxers hand out ----

static int isStartCode(const uint8_t *d, int size, int i, int *codeLength)
{
   if (i + 4 <= size && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) { *codeLength = 4; return 1; }
   if (i + 3 <= size && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)                  { *codeLength = 3; return 1; }
   return 0;
}

// index of the first NAL's first byte, or -1 when the buffer holds no start code
static int findFirstNal(const uint8_t *d, int size)
{
   int i = 0, code;
   while (i < size && !isStartCode(d, size, i, &code)) i++;
   return i >= size ? -1 : i + code;
}

static int getLengthPrefixedSize(const uint8_t *annexB, int size)
{
   int nalStart = findFirstNal(annexB, size);
   if (nalStart < 0) return -1;

   int i = nalStart, code, total = 0;
   while (i < size) {
      if (isStartCode(annexB, size, i, &code)) { total += 4 + (i - nalStart); i += code; nalStart = i; }
      else i++;
   }
   if (size > nalStart) total += 4 + (size - nalStart);
   return total;
}

// append one [4-byte length][nal] to out at *written, if it fits
static int putNal(const uint8_t *nal, int length, uint8_t *out, int capacity, int *written)
{
   if (*written + 4 + length > capacity) return -1;
   out[(*written)++] = (uint8_t)(length >> 24);
   out[(*written)++] = (uint8_t)(length >> 16);
   out[(*written)++] = (uint8_t)(length >> 8);
   out[(*written)++] = (uint8_t)length;
   memcpy(out + *written, nal, length);
   *written += length;
   return 0;
}

static int toLengthPrefixed(const uint8_t *annexB, int size, uint8_t *out, int capacity)
{
   int nalStart = findFirstNal(annexB, size);
   if (nalStart < 0) return -1;

   int i = nalStart, code, written = 0;
   while (i < size) {
      if (isStartCode(annexB, size, i, &code)) {
         if (putNal(annexB + nalStart, i - nalStart, out, capacity, &written)) return -1;
         i += code;
         nalStart = i;
      } else i++;
   }
   if (size > nalStart && putNal(annexB + nalStart, size - nalStart, out, capacity, &written)) return -1;
   return written;
}

static int buildAvcc(const H264Config *h264, uint8_t *out, int capacity)
{
   // section: find the SPS and PPS in the Annex-B header
   const uint8_t *sps = NULL, *pps = NULL;
   int spsLength = 0, ppsLength = 0;
   const uint8_t *header = h264->header;
   int size = h264->headerSize;

   int nalStart = findFirstNal(header, size);
   if (nalStart < 0) return -1;

   int i = nalStart, code;
   for (;;) {
      int atEnd = i >= size;
      if (atEnd || isStartCode(header, size, i, &code)) {
         const uint8_t *nal = header + nalStart;
         int nalLength = (atEnd ? size : i) - nalStart;
         if (nalLength > 0) {
            int type = nal[0] & 0x1F;
            if (type == 7 && !sps) { sps = nal; spsLength = nalLength; }
            else if (type == 8 && !pps) { pps = nal; ppsLength = nalLength; }
         }
         if (atEnd) break;
         i += code;
         nalStart = i;
      } else i++;
   }
   if (!sps || !pps || spsLength < 4 || 11 + spsLength + ppsLength > capacity) return -1;

   // section: assemble the record
   int n = 0;
   out[n++] = 1;                        // configurationVersion
   out[n++] = sps[1];                   // AVCProfileIndication
   out[n++] = sps[2];                   // profile_compatibility
   out[n++] = sps[3];                   // AVCLevelIndication
   out[n++] = 0xFF;                     // 111111 + lengthSizeMinusOne (3 -> 4-byte lengths)
   out[n++] = 0xE1;                     // 111 + numOfSequenceParameterSets (1)
   out[n++] = (uint8_t)(spsLength >> 8); out[n++] = (uint8_t)spsLength;
   memcpy(out + n, sps, spsLength); n += spsLength;
   out[n++] = 1;                        // numOfPictureParameterSets
   out[n++] = (uint8_t)(ppsLength >> 8); out[n++] = (uint8_t)ppsLength;
   memcpy(out + n, pps, ppsLength); n += ppsLength;
   return n;
}

static void buildAacAsc(int rate, int channels, uint8_t asc[2])
{
   static const int table[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
   int index = 4;   // default 44100
   for (int i = 0; i < 13; i++) if (table[i] == rate) { index = i; break; }
   asc[0] = (uint8_t)((2 << 3) | ((index >> 1) & 0x07));
   asc[1] = (uint8_t)(((index & 1) << 7) | ((channels & 0x0F) << 3));
}

// ---- growable byte buffer (the moov box is assembled in memory, then written in one go) ----

typedef struct {
   uint8_t *data;
   int      length, capacity;
   int      outOfMemory;
} ByteBuffer;

static void putBytes(ByteBuffer *buffer, const void *data, int length)
{
   if (buffer->outOfMemory) return;
   if (buffer->length + length > buffer->capacity) {
      int capacity = buffer->capacity ? buffer->capacity : 4096;
      while (capacity < buffer->length + length) capacity *= 2;
      uint8_t *grown = (uint8_t *)realloc(buffer->data, capacity);
      if (!grown) { buffer->outOfMemory = 1; return; }
      buffer->data = grown;
      buffer->capacity = capacity;
   }
   memcpy(buffer->data + buffer->length, data, length);
   buffer->length += length;
}

static void putU8(ByteBuffer *buffer, uint8_t value) { putBytes(buffer, &value, 1); }

static void putU16(ByteBuffer *buffer, uint32_t value)
{
   uint8_t raw[2] = { (uint8_t)(value >> 8), (uint8_t)value };
   putBytes(buffer, raw, 2);
}

static void putU32(ByteBuffer *buffer, uint32_t value)
{
   uint8_t raw[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };
   putBytes(buffer, raw, 4);
}

static void putU64(ByteBuffer *buffer, uint64_t value)
{
   putU32(buffer, (uint32_t)(value >> 32));
   putU32(buffer, (uint32_t)value);
}

static void putZeros(ByteBuffer *buffer, int count)
{
   uint8_t zero[16] = { 0 };
   while (count > 0) { int chunk = count > 16 ? 16 : count; putBytes(buffer, zero, chunk); count -= chunk; }
}

// boxes carry their own total size, which is only known once the children are in: write a placeholder,
// then patch it when the box ends.
static int beginBox(ByteBuffer *buffer, const char *type)
{
   int start = buffer->length;
   putU32(buffer, 0);
   putBytes(buffer, type, 4);
   return start;
}

static void patchU32(ByteBuffer *buffer, int offset, uint32_t value)
{
   if (buffer->outOfMemory) return;
   buffer->data[offset]     = (uint8_t)(value >> 24);
   buffer->data[offset + 1] = (uint8_t)(value >> 16);
   buffer->data[offset + 2] = (uint8_t)(value >> 8);
   buffer->data[offset + 3] = (uint8_t)value;
}

static void endBox(ByteBuffer *buffer, int start)
{
   patchU32(buffer, start, (uint32_t)(buffer->length - start));
}

static int beginFullBox(ByteBuffer *buffer, const char *type, uint8_t version, uint32_t flags)
{
   int start = beginBox(buffer, type);
   putU32(buffer, ((uint32_t)version << 24) | (flags & 0x00FFFFFF));
   return start;
}

// the unity display matrix every track carries in tkhd / mvhd
static void putUnityMatrix(ByteBuffer *buffer)
{
   putU32(buffer, 0x00010000); putU32(buffer, 0); putU32(buffer, 0);
   putU32(buffer, 0); putU32(buffer, 0x00010000); putU32(buffer, 0);
   putU32(buffer, 0); putU32(buffer, 0); putU32(buffer, 0x40000000);
}

// ---- sample tables ----

static int growTrack(Mp4Track *track)
{
   if (track->count < track->capacity) return 0;
   int capacity = track->capacity ? track->capacity * 2 : 4096;
   uint64_t *offsets = (uint64_t *)realloc(track->offsets, capacity * sizeof *offsets);
   if (offsets) track->offsets = offsets;
   uint32_t *sizes = (uint32_t *)realloc(track->sizes, capacity * sizeof *sizes);
   if (sizes) track->sizes = sizes;
   uint32_t *times = (uint32_t *)realloc(track->times, capacity * sizeof *times);
   if (times) track->times = times;
   if (!offsets || !sizes || !times) { track->outOfMemory = 1; return -1; }
   track->capacity = capacity;
   return 0;
}

static int addSample(Mp4Track *track, uint64_t offset, uint32_t size, uint32_t timeTicks, int keyframe)
{
   if (growTrack(track)) return -1;
   track->offsets[track->count] = offset;
   track->sizes[track->count]   = size;
   track->times[track->count]   = timeTicks;
   track->count++;

   if (!keyframe) return 0;
   if (track->syncCount >= track->syncCapacity) {
      int capacity = track->syncCapacity ? track->syncCapacity * 2 : 256;
      uint32_t *grown = (uint32_t *)realloc(track->syncSamples, capacity * sizeof *grown);
      if (!grown) { track->outOfMemory = 1; return -1; }
      track->syncSamples = grown;
      track->syncCapacity = capacity;
   }
   track->syncSamples[track->syncCount++] = (uint32_t)track->count;   // stss indices are 1-based
   return 0;
}

static void freeTrack(Mp4Track *track)
{
   free(track->offsets); free(track->sizes); free(track->times); free(track->syncSamples);
   memset(track, 0, sizeof *track);
}

static int compareTicks(const void *left, const void *right)
{
   uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
   return a < b ? -1 : a > b ? 1 : 0;
}

// Derive decode times from presentation times. The demuxers only give presentation times, but the samples
// arrive in decode order, so sorting the times gives the decode timeline. B-frames present later than they
// decode, so the whole track is delayed by the largest reorder gap and each sample carries the difference
// as its composition offset. Returns the delay in ticks; fills decodeTimes (which the caller owns).
static uint32_t buildDecodeTimes(const uint32_t *times, int count, uint32_t *decodeTimes)
{
   memcpy(decodeTimes, times, count * sizeof *decodeTimes);
   qsort(decodeTimes, count, sizeof *decodeTimes, compareTicks);

   uint32_t delay = 0;
   for (int i = 0; i < count; i++)
      if (decodeTimes[i] > times[i] && decodeTimes[i] - times[i] > delay) delay = decodeTimes[i] - times[i];
   return delay;
}

// ---- sample table boxes ----

// stts and ctts are both an entry count followed by run-length encoded (repeat, value) pairs.
typedef struct {
   ByteBuffer *buffer;
   int         boxStart, entryCountOffset;
   uint32_t    entries, runValue, runCount;
} RunEncoder;

static void beginRuns(RunEncoder *runs, ByteBuffer *buffer, const char *type)
{
   memset(runs, 0, sizeof *runs);
   runs->buffer = buffer;
   runs->boxStart = beginFullBox(buffer, type, 0, 0);
   runs->entryCountOffset = buffer->length;
   putU32(buffer, 0);
}

static void flushRun(RunEncoder *runs)
{
   if (!runs->runCount) return;
   putU32(runs->buffer, runs->runCount);
   putU32(runs->buffer, runs->runValue);
   runs->entries++;
   runs->runCount = 0;
}

static void putRun(RunEncoder *runs, uint32_t value)
{
   if (runs->runCount && value == runs->runValue) { runs->runCount++; return; }
   flushRun(runs);
   runs->runValue = value;
   runs->runCount = 1;
}

static void endRuns(RunEncoder *runs)
{
   flushRun(runs);
   patchU32(runs->buffer, runs->entryCountOffset, runs->entries);
   endBox(runs->buffer, runs->boxStart);
}

// stts: how long each sample holds the decoder. the last sample has no successor to measure against, so it
// reuses the previous duration. returns the total, which is the track's media duration.
static uint32_t putSttsBox(ByteBuffer *buffer, const uint32_t *decodeTimes, int count, uint32_t fallbackDuration)
{
   RunEncoder runs;
   beginRuns(&runs, buffer, "stts");

   uint32_t total = 0, previous = fallbackDuration;
   for (int i = 0; i < count; i++) {
      uint32_t duration = (i + 1 < count) ? decodeTimes[i + 1] - decodeTimes[i] : previous;
      putRun(&runs, duration);
      previous = duration;
      total += duration;
   }
   endRuns(&runs);
   return total;
}

// ctts: how far each sample presents after it decodes. skipped when nothing reorders.
static void putCttsBox(ByteBuffer *buffer, const uint32_t *times, const uint32_t *decodeTimes, int count, uint32_t delay)
{
   if (!delay) return;

   RunEncoder runs;
   beginRuns(&runs, buffer, "ctts");
   for (int i = 0; i < count; i++) putRun(&runs, times[i] + delay - decodeTimes[i]);
   endRuns(&runs);
}

static void putStssBox(ByteBuffer *buffer, const Mp4Track *track)
{
   if (track->syncCount == 0) return;
   int start = beginFullBox(buffer, "stss", 0, 0);
   putU32(buffer, (uint32_t)track->syncCount);
   for (int i = 0; i < track->syncCount; i++) putU32(buffer, track->syncSamples[i]);
   endBox(buffer, start);
}

// every sample is its own chunk, so the chunk offsets are just the sample offsets.
static void putChunkBoxes(ByteBuffer *buffer, const Mp4Track *track)
{
   int start = beginFullBox(buffer, "stsc", 0, 0);
   putU32(buffer, 1);
   putU32(buffer, 1);   // first_chunk
   putU32(buffer, 1);   // samples_per_chunk
   putU32(buffer, 1);   // sample_description_index
   endBox(buffer, start);

   start = beginFullBox(buffer, "stsz", 0, 0);
   putU32(buffer, 0);   // 0 = sizes vary, one entry per sample
   putU32(buffer, (uint32_t)track->count);
   for (int i = 0; i < track->count; i++) putU32(buffer, track->sizes[i]);
   endBox(buffer, start);

   int needs64Bit = track->count > 0 && track->offsets[track->count - 1] > 0xFFFFFFFFull;
   start = beginFullBox(buffer, needs64Bit ? "co64" : "stco", 0, 0);
   putU32(buffer, (uint32_t)track->count);
   for (int i = 0; i < track->count; i++) {
      if (needs64Bit) putU64(buffer, track->offsets[i]);
      else            putU32(buffer, (uint32_t)track->offsets[i]);
   }
   endBox(buffer, start);
}

// ---- sample description boxes ----

static void putAvc1Box(ByteBuffer *buffer, const Mp4Muxer *mux)
{
   int start = beginBox(buffer, "avc1");
   putZeros(buffer, 6);
   putU16(buffer, 1);            // data_reference_index
   putZeros(buffer, 16);         // pre_defined + reserved
   putU16(buffer, (uint32_t)mux->width);
   putU16(buffer, (uint32_t)mux->height);
   putU32(buffer, 0x00480000);   // horizontal resolution, 72 dpi
   putU32(buffer, 0x00480000);   // vertical resolution, 72 dpi
   putU32(buffer, 0);            // reserved
   putU16(buffer, 1);            // frame_count
   putZeros(buffer, 32);         // compressorname
   putU16(buffer, 0x0018);       // depth
   putU16(buffer, 0xFFFF);       // pre_defined
   int avcc = beginBox(buffer, "avcC");
   putBytes(buffer, mux->avcc, mux->avccLength);
   endBox(buffer, avcc);
   endBox(buffer, start);
}

// esds: the MPEG-4 descriptor chain that names the codec (AAC-LC) and carries its AudioSpecificConfig.
static void putEsdsBox(ByteBuffer *buffer, const Mp4Muxer *mux)
{
   uint8_t asc[2];
   buildAacAsc(mux->audioRate, mux->audioChannels, asc);

   int start = beginFullBox(buffer, "esds", 0, 0);
   putU8(buffer, 0x03);                       // ES_Descriptor
   putU8(buffer, (uint8_t)(3 + (2 + 13 + 2 + sizeof asc) + (2 + 1)));
   putU16(buffer, 1);                         // ES_ID
   putU8(buffer, 0);                          // no dependency, no url, no OCR, priority 0

   putU8(buffer, 0x04);                       // DecoderConfigDescriptor
   putU8(buffer, (uint8_t)(13 + 2 + sizeof asc));
   putU8(buffer, 0x40);                       // objectTypeIndication: MPEG-4 audio
   putU8(buffer, 0x15);                       // streamType: audio, not upstream
   putZeros(buffer, 3);                       // bufferSizeDB
   putU32(buffer, 0);                         // maxBitrate, unknown
   putU32(buffer, 0);                         // avgBitrate, unknown

   putU8(buffer, 0x05);                       // DecoderSpecificInfo
   putU8(buffer, (uint8_t)sizeof asc);
   putBytes(buffer, asc, sizeof asc);

   putU8(buffer, 0x06);                       // SLConfigDescriptor
   putU8(buffer, 1);
   putU8(buffer, 0x02);                       // predefined: MP4 timestamps
   endBox(buffer, start);
}

static void putMp4aBox(ByteBuffer *buffer, const Mp4Muxer *mux)
{
   int start = beginBox(buffer, "mp4a");
   putZeros(buffer, 6);
   putU16(buffer, 1);            // data_reference_index
   putZeros(buffer, 8);          // reserved
   putU16(buffer, (uint32_t)mux->audioChannels);
   putU16(buffer, 16);           // sample size in bits
   putU16(buffer, 0);            // pre_defined
   putU16(buffer, 0);            // reserved
   putU32(buffer, (uint32_t)mux->audioRate << 16);
   putEsdsBox(buffer, mux);
   endBox(buffer, start);
}

// ---- track boxes ----

static void putTkhdBox(ByteBuffer *buffer, const Mp4Muxer *mux, int isVideo, uint32_t movieDuration)
{
   int start = beginFullBox(buffer, "tkhd", 0, 0x000007);   // enabled, in movie, in preview
   putU32(buffer, 0);            // creation time
   putU32(buffer, 0);            // modification time
   putU32(buffer, isVideo ? 1 : 2);   // track id
   putU32(buffer, 0);            // reserved
   putU32(buffer, movieDuration);
   putZeros(buffer, 8);          // reserved
   putU16(buffer, 0);            // layer
   putU16(buffer, 0);            // alternate_group
   putU16(buffer, isVideo ? 0 : 0x0100);   // volume: full for audio, none for video
   putU16(buffer, 0);            // reserved
   putUnityMatrix(buffer);
   putU32(buffer, isVideo ? (uint32_t)mux->width << 16 : 0);
   putU32(buffer, isVideo ? (uint32_t)mux->height << 16 : 0);
   endBox(buffer, start);
}

static void putMdhdBox(ByteBuffer *buffer, uint32_t timescale, uint32_t duration)
{
   int start = beginFullBox(buffer, "mdhd", 0, 0);
   putU32(buffer, 0);            // creation time
   putU32(buffer, 0);            // modification time
   putU32(buffer, timescale);
   putU32(buffer, duration);
   putU16(buffer, 0x55C4);       // language: "und"
   putU16(buffer, 0);            // pre_defined
   endBox(buffer, start);
}

static void putHdlrBox(ByteBuffer *buffer, const char *handlerType, const char *name)
{
   int start = beginFullBox(buffer, "hdlr", 0, 0);
   putU32(buffer, 0);            // pre_defined
   putBytes(buffer, handlerType, 4);
   putZeros(buffer, 12);         // reserved
   putBytes(buffer, name, (int)strlen(name) + 1);
   endBox(buffer, start);
}

// dinf/dref: one entry saying the media lives in this same file
static void putDinfBox(ByteBuffer *buffer)
{
   int dinf = beginBox(buffer, "dinf");
   int dref = beginFullBox(buffer, "dref", 0, 0);
   putU32(buffer, 1);
   int url = beginFullBox(buffer, "url ", 0, 1);   // flag 1 = self-contained
   endBox(buffer, url);
   endBox(buffer, dref);
   endBox(buffer, dinf);
}

// one trak box: header, media header, and the sample tables. isVideo picks the video-specific children.
static uint32_t putTrakBox(ByteBuffer *buffer, const Mp4Muxer *mux, const Mp4Track *track, int isVideo)
{
   uint32_t *decodeTimes = (uint32_t *)malloc(track->count * sizeof *decodeTimes);
   if (!decodeTimes) { buffer->outOfMemory = 1; return 0; }
   uint32_t delay = buildDecodeTimes(track->times, track->count, decodeTimes);

   uint32_t timescale = isVideo ? VIDEO_TIMESCALE : (uint32_t)mux->audioRate;
   uint32_t fallbackDuration = isVideo ? (mux->frameDurationTicks ? mux->frameDurationTicks : VIDEO_TIMESCALE / 30)
                                       : AAC_FRAME_SAMPLES;

   // the sample tables have to be built before the headers, which need the duration they add up to
   ByteBuffer stbl = { 0 };
   int stblStart = beginBox(&stbl, "stbl");
   int stsd = beginFullBox(&stbl, "stsd", 0, 0);
   putU32(&stbl, 1);
   if (isVideo) putAvc1Box(&stbl, mux); else putMp4aBox(&stbl, mux);
   endBox(&stbl, stsd);
   uint32_t mediaDuration = putSttsBox(&stbl, decodeTimes, track->count, fallbackDuration);
   putCttsBox(&stbl, track->times, decodeTimes, track->count, delay);
   if (isVideo) putStssBox(&stbl, track);
   putChunkBoxes(&stbl, track);
   endBox(&stbl, stblStart);
   free(decodeTimes);

   mediaDuration += delay;
   uint32_t movieDuration = (uint32_t)((uint64_t)mediaDuration * MOVIE_TIMESCALE / timescale);

   int trak = beginBox(buffer, "trak");
   putTkhdBox(buffer, mux, isVideo, movieDuration);
   int mdia = beginBox(buffer, "mdia");
   putMdhdBox(buffer, timescale, mediaDuration);
   putHdlrBox(buffer, isVideo ? "vide" : "soun", isVideo ? "VideoHandler" : "SoundHandler");
   int minf = beginBox(buffer, "minf");
   if (isVideo) {
      int vmhd = beginFullBox(buffer, "vmhd", 0, 1);
      putZeros(buffer, 8);       // graphicsmode + opcolor
      endBox(buffer, vmhd);
   } else {
      int smhd = beginFullBox(buffer, "smhd", 0, 0);
      putZeros(buffer, 4);       // balance + reserved
      endBox(buffer, smhd);
   }
   putDinfBox(buffer);
   putBytes(buffer, stbl.data, stbl.length);
   endBox(buffer, minf);
   endBox(buffer, mdia);
   endBox(buffer, trak);

   if (stbl.outOfMemory) buffer->outOfMemory = 1;
   free(stbl.data);
   return movieDuration;
}

static void putMvhdBox(ByteBuffer *buffer, uint32_t duration, int trackCount)
{
   int start = beginFullBox(buffer, "mvhd", 0, 0);
   putU32(buffer, 0);            // creation time
   putU32(buffer, 0);            // modification time
   putU32(buffer, MOVIE_TIMESCALE);
   putU32(buffer, duration);
   putU32(buffer, 0x00010000);   // rate: 1.0
   putU16(buffer, 0x0100);       // volume: full
   putU16(buffer, 0);            // reserved
   putZeros(buffer, 8);          // reserved
   putUnityMatrix(buffer);
   putZeros(buffer, 24);         // pre_defined
   putU32(buffer, (uint32_t)trackCount + 1);
   endBox(buffer, start);
}

// ---- file writing ----

static int writeChunk(Mp4Muxer *mux, const void *data, int length)
{
   if (writeFs(&mux->file, data, length) != length) return -1;
   mux->bytesWritten += length;
   return 0;
}

static int writeFtypBox(Mp4Muxer *mux)
{
   ByteBuffer buffer = { 0 };
   int start = beginBox(&buffer, "ftyp");
   putBytes(&buffer, "isom", 4);
   putU32(&buffer, 0x200);
   putBytes(&buffer, "isom", 4);
   putBytes(&buffer, "iso2", 4);
   putBytes(&buffer, "avc1", 4);
   putBytes(&buffer, "mp41", 4);
   endBox(&buffer, start);

   int rc = buffer.outOfMemory ? -1 : writeChunk(mux, buffer.data, buffer.length);
   free(buffer.data);
   return rc;
}

// mdat is opened with the 64-bit size form so the real size can be patched in at close whatever it is.
static int writeMdatHeader(Mp4Muxer *mux)
{
   uint8_t header[16] = { 0, 0, 0, 1, 'm', 'd', 'a', 't' };
   return writeChunk(mux, header, sizeof header);
}

static int patchMdatSize(Mp4Muxer *mux, uint64_t mdatSize)
{
   uint8_t size[8];
   for (int i = 0; i < 8; i++) size[i] = (uint8_t)(mdatSize >> (8 * (7 - i)));
   if (seekFs(&mux->file, (int64_t)mux->mdatStart + 8, VFS_SEEK_SET) < 0) return -1;
   return writeFs(&mux->file, size, sizeof size) == sizeof size ? 0 : -1;
}

static int writeMoovBox(Mp4Muxer *mux)
{
   int trackCount = (mux->hasAudio && mux->audio.count > 0) ? 2 : 1;

   // the tracks are built first because mvhd, which comes before them, needs the longest track's duration
   ByteBuffer tracks = { 0 };
   uint32_t duration = putTrakBox(&tracks, mux, &mux->video, 1);
   if (trackCount == 2) {
      uint32_t audioDuration = putTrakBox(&tracks, mux, &mux->audio, 0);
      if (audioDuration > duration) duration = audioDuration;
   }

   ByteBuffer buffer = { 0 };
   int start = beginBox(&buffer, "moov");
   putMvhdBox(&buffer, duration, trackCount);
   putBytes(&buffer, tracks.data, tracks.length);
   endBox(&buffer, start);

   int rc = (tracks.outOfMemory || buffer.outOfMemory) ? -1 : writeChunk(mux, buffer.data, buffer.length);
   free(tracks.data);
   free(buffer.data);
   return rc;
}

// ---- public interface ----

int openMp4Muxer(Mp4Muxer *mux, const char *path, const H264Config *h264, int width, int height,
                 uint64_t frameDurationNs, int audioRate, int audioChannels)
{
   memset(mux, 0, sizeof *mux);
   mux->width = width;
   mux->height = height;
   mux->audioRate = audioRate;
   mux->audioChannels = audioChannels;
   mux->hasAudio = (audioRate > 0 && audioChannels > 0);
   mux->frameDurationTicks = nsToTicks(frameDurationNs, VIDEO_TIMESCALE);

   mux->avccLength = buildAvcc(h264, mux->avcc, sizeof mux->avcc);
   if (mux->avccLength < 0) { logError("[mp4] avcC build failed\n"); return -1; }

   if (openFs(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &mux->file) != 0) { logError("[mp4] open '%s' failed\n", path); return -1; }
   mux->isOpen = 1;

   if (writeFtypBox(mux)) { closeFs(&mux->file); mux->isOpen = 0; deleteFile(path); return -1; }
   mux->mdatStart = mux->bytesWritten;
   if (writeMdatHeader(mux)) { closeFs(&mux->file); mux->isOpen = 0; deleteFile(path); return -1; }
   return 0;
}

int writeMp4Video(Mp4Muxer *mux, const uint8_t *annexB, int size, uint64_t ptsNs, int keyframe)
{
   int payloadLength = getLengthPrefixedSize(annexB, size);
   if (payloadLength < 0) return -1;

   if (payloadLength > mux->sampleCapacity) {
      uint8_t *grown = (uint8_t *)realloc(mux->sample, payloadLength);
      if (!grown) return -1;
      mux->sample = grown;
      mux->sampleCapacity = payloadLength;
   }
   if (toLengthPrefixed(annexB, size, mux->sample, mux->sampleCapacity) != payloadLength) return -1;

   uint64_t offset = mux->bytesWritten;
   if (writeChunk(mux, mux->sample, payloadLength)) return -1;
   return addSample(&mux->video, offset, (uint32_t)payloadLength, nsToTicks(ptsNs, VIDEO_TIMESCALE), keyframe);
}

int writeMp4Audio(Mp4Muxer *mux, const uint8_t *aac, int size, uint64_t ptsNs)
{
   if (size <= 0) return 0;
   uint64_t offset = mux->bytesWritten;
   if (writeChunk(mux, aac, size)) return -1;
   return addSample(&mux->audio, offset, (uint32_t)size, nsToTicks(ptsNs, (uint32_t)mux->audioRate), 0);
}

int closeMp4Muxer(Mp4Muxer *mux)
{
   int rc = 0;
   if (mux->isOpen) {
      uint64_t mdatSize = mux->bytesWritten - mux->mdatStart;
      if (mux->video.count == 0 || mux->video.outOfMemory || mux->audio.outOfMemory) rc = -1;
      if (!rc && writeMoovBox(mux)) rc = -1;
      if (!rc && patchMdatSize(mux, mdatSize)) rc = -1;
      if (closeFs(&mux->file) != 0) rc = -1;
      mux->isOpen = 0;
   }
   freeTrack(&mux->video);
   freeTrack(&mux->audio);
   free(mux->sample);
   mux->sample = NULL;
   mux->sampleCapacity = 0;
   return rc;
}
