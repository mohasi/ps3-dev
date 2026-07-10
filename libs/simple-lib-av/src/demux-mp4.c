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
#define MAX_FRAG_SAMPLES   8192                  // per fragment (a fragment is a few seconds)
#define MAX_MOOF_BYTES     (1 * 1024 * 1024)     // sanity cap when reading one moof box
#define MAX_SIDX_BYTES     (256 * 1024)          // sanity cap when reading the segment index box

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

// advances past one MP4 descriptor's expandable length field (1-4 bytes, high bit = continue)
static int skipDescriptorLen(const uint8_t *data, int len, int pos)
{
   for (int i = 0; i < 4 && pos < len; i++) if (!(data[pos++] & 0x80)) break;
   return pos;
}

// returns the AAC AudioObjectType from an esds box payload, or -1. AOT 5 (SBR) / 29 (PS) mean HE-AAC,
// whose core frames run at half the reported sample rate. Walks ES_Descriptor -> DecoderConfig ->
// DecoderSpecificInfo (the AudioSpecificConfig, whose top 5 bits are the AOT).
static int getAacObjectType(const uint8_t *esds, int len)
{
   int pos = 4;   // skip the esds fullbox version/flags
   if (pos >= len || esds[pos++] != 0x03) return -1;   // ES_Descriptor
   pos = skipDescriptorLen(esds, len, pos) + 3;        // + ES_ID(2) + flags(1), assuming no dependency/URL/OCR
   if (pos >= len || esds[pos++] != 0x04) return -1;   // DecoderConfigDescriptor
   pos = skipDescriptorLen(esds, len, pos) + 13;       // + objectTypeIndication(1)+streamType/buffer(4)+max/avgBitrate(8)
   if (pos >= len || esds[pos++] != 0x05) return -1;   // DecoderSpecificInfo (AudioSpecificConfig)
   pos = skipDescriptorLen(esds, len, pos);
   if (pos >= len) return -1;
   int aot = esds[pos] >> 3;
   if (aot == 31 && pos + 1 < len) aot = 32 + (((esds[pos] & 7) << 3) | (esds[pos + 1] >> 5));
   return aot;
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
      demuxer->videoTimescale = timescale;   // fragmented converts pts at read time
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

      // esds AudioSpecificConfig: HE-AAC (SBR) reports the doubled output rate here, but its raw
      // frames are the AAC-LC core at half that, so the ADTS header must carry the core rate.
      int adtsRate = rate;
      uint64_t esdsStart, esdsEnd;
      if (findMp4ChildBox(source, entryPos + 8 + 28, entryEnd, FOURCC('e','s','d','s'), &esdsStart, &esdsEnd) == 0) {
         uint8_t esds[64];
         int n = (int)(esdsEnd - esdsStart);
         if (n > (int)sizeof esds) n = sizeof esds;
         int aot = (n > 4 && readVideoSourceAt(source, esdsStart, esds, n) == 0) ? getAacObjectType(esds, n) : -1;
         if (aot == 5 || aot == 29) adtsRate = rate / 2;
      }

      // plain files carry the sample table here; a fragmented (audio-only) file has none and loads
      // samples per moof later, so a NULL table is fine as long as we captured rate/channels/timescale
      demuxer->audioSamples   = buildSampleTable(source, stblStart, stblEnd, timescale, 0, &demuxer->audioSampleCount, 0);
      demuxer->hasAudio       = 1;
      demuxer->audioRate      = rate;
      demuxer->audioAdtsRate  = adtsRate;
      demuxer->audioChannels  = channels;
      demuxer->audioTimescale = timescale;
      logInfo("[demux-mp4] aac track, %d Hz (adts %d), %d ch, %d samples\n", rate, adtsRate, channels, demuxer->audioSampleCount);
   }
}

// ============================================================================
// fragmented (moof) support
// ============================================================================

// finds child box `type` within an in-memory buffer window [start, end). fills the child's payload
// range (buffer-relative). 0 / -1.
static int findBoxInBuffer(const uint8_t *buffer, uint32_t start, uint32_t end, uint32_t type, uint32_t *payloadStart, uint32_t *payloadEnd)
{
   uint32_t pos = start;
   while (pos + 8 <= end) {
      uint32_t size = readU32BE(buffer + pos);
      uint32_t boxType = readU32BE(buffer + pos + 4);
      uint32_t headerLen = 8;
      if (size == 1) { if (pos + 16 > end) return -1; size = (uint32_t)readU64BE(buffer + pos + 8); headerLen = 16; }
      if (size < headerLen || pos + size > end) return -1;
      if (boxType == type) { *payloadStart = pos + headerLen; *payloadEnd = pos + size; return 0; }
      pos += size;
   }
   return -1;
}

// reads the next moof (from nextMoofPos, skipping any styp/sidx), parses its single track fragment's
// trun into `samples` (up to maxSamples) using `timescale`, and advances nextMoofPos past this
// fragment's mdat. frameDurationOut, when non-NULL, receives the first sample's duration in ns.
// returns the sample count (0 at end of stream). Track-agnostic: the caller picks video or audio.
static int loadFragmentInto(Mp4Demuxer *demuxer, Mp4Sample *samples, int maxSamples, uint64_t timescale, uint64_t *frameDurationOut)
{
   VideoSource *source = &demuxer->source;

   // find the next moof, skipping non-moof boxes (styp/sidx). these reads are sequential from where
   // playback left off, so they stay in the source's window - no large seek.
   uint64_t moofStart = demuxer->nextMoofPos, moofSize = 0, headerLen = 0;
   for (int guard = 0; guard < 16; guard++) {
      uint32_t type;
      if (demuxer->fileEnd && moofStart + 8 > demuxer->fileEnd) return 0;   // fileEnd 0 = unknown: let the read hit EOF
      if (readMp4Box(source, moofStart, &type, &moofSize, &headerLen) != 0 || moofSize == 0) return 0;
      if (type == FOURCC('m','o','o','f')) break;
      moofStart += moofSize;
   }
   if (moofSize < 8 || moofSize > MAX_MOOF_BYTES) return 0;

   uint8_t *moof = (uint8_t *)malloc(moofSize);
   if (!moof) return 0;
   if (readVideoSourceAt(source, moofStart, moof, moofSize) != 0) { free(moof); return 0; }

   // traf -> tfhd (defaults + base), tfdt (decode time), trun (per-sample table)
   uint32_t trafStart, trafEnd, tfhdStart, tfhdEnd, trunStart, trunEnd, tfdtStart, tfdtEnd;
   if (findBoxInBuffer(moof, (uint32_t)headerLen, (uint32_t)moofSize, FOURCC('t','r','a','f'), &trafStart, &trafEnd) != 0 ||
       findBoxInBuffer(moof, trafStart, trafEnd, FOURCC('t','f','h','d'), &tfhdStart, &tfhdEnd) != 0 ||
       findBoxInBuffer(moof, trafStart, trafEnd, FOURCC('t','r','u','n'), &trunStart, &trunEnd) != 0) {
      free(moof);
      return 0;
   }

   // tfhd: flags decide which optional fields (base offset, per-sample defaults) are present
   uint32_t tfhdFlags = readU32BE(moof + tfhdStart) & 0xFFFFFF;
   uint32_t p = tfhdStart + 4 + 4;   // skip version/flags + track_id (single track: id ignored)
   uint64_t baseOffset = moofStart;
   if (tfhdFlags & 0x000001) { baseOffset = readU64BE(moof + p); p += 8; }   // base_data_offset (else default-base-is-moof)
   if (tfhdFlags & 0x000002) p += 4;                                          // sample_description_index
   uint32_t defDuration = demuxer->trexDuration, defSize = demuxer->trexSize, defFlags = demuxer->trexFlags;
   if (tfhdFlags & 0x000008) { defDuration = readU32BE(moof + p); p += 4; }
   if (tfhdFlags & 0x000010) { defSize     = readU32BE(moof + p); p += 4; }
   if (tfhdFlags & 0x000020) { defFlags    = readU32BE(moof + p); p += 4; }

   // tfdt: base media decode time for this fragment (optional; 0 if absent)
   uint64_t dts = 0;
   if (findBoxInBuffer(moof, trafStart, trafEnd, FOURCC('t','f','d','t'), &tfdtStart, &tfdtEnd) == 0)
      dts = moof[tfdtStart] == 1 ? readU64BE(moof + tfdtStart + 4) : readU32BE(moof + tfdtStart + 4);

   // trun: sample count, data offset, and per-sample fields per its flags
   uint32_t trunFlags = readU32BE(moof + trunStart) & 0xFFFFFF;
   uint32_t q = trunStart + 4;
   uint32_t sampleCount = readU32BE(moof + q); q += 4;
   int32_t  dataOffset = 0;
   uint32_t firstSampleFlags = 0;
   if (trunFlags & 0x000001) { dataOffset = (int32_t)readU32BE(moof + q); q += 4; }
   if (trunFlags & 0x000004) { firstSampleFlags = readU32BE(moof + q); q += 4; }

   // bytes each sample actually reads from the trun, per its present-field flags. a compact fragment
   // that leaves duration/size/flags to the trex defaults has a per-sample width under 16 (or 0) - the
   // old fixed `q + 16` guard then dropped every sample of such a moof, yielding an empty fragment.
   uint32_t sampleBytes = 0;
   if (trunFlags & 0x000100) sampleBytes += 4;
   if (trunFlags & 0x000200) sampleBytes += 4;
   if (trunFlags & 0x000400) sampleBytes += 4;
   if (trunFlags & 0x000800) sampleBytes += 4;

   uint64_t sampleOffset = baseOffset + (uint64_t)(int64_t)dataOffset;
   if (sampleCount > (uint32_t)maxSamples) sampleCount = (uint32_t)maxSamples;
   uint32_t count = 0;
   for (uint32_t i = 0; i < sampleCount; i++) {
      if (q + sampleBytes > trunEnd) break;   // stop only when the trun can't hold another sample's fields
      uint32_t duration = defDuration, size = defSize;
      uint32_t flags = (i == 0 && (trunFlags & 0x000004)) ? firstSampleFlags : defFlags;
      int32_t  compositionOffset = 0;
      if (trunFlags & 0x000100) { duration = readU32BE(moof + q); q += 4; }
      if (trunFlags & 0x000200) { size     = readU32BE(moof + q); q += 4; }
      if (trunFlags & 0x000400) { flags    = readU32BE(moof + q); q += 4; }
      if (trunFlags & 0x000800) { compositionOffset = (int32_t)readU32BE(moof + q); q += 4; }

      Mp4Sample *sample = &samples[count++];
      sample->offset   = sampleOffset;
      sample->size     = size;
      int64_t composition = (int64_t)dts + compositionOffset;
      sample->ptsNs    = composition > 0 ? ticksToNs((uint64_t)composition, timescale) : 0;
      sample->keyframe = (flags & 0x00010000) ? 0 : 1;   // sample_is_non_sync_sample bit
      sampleOffset += size;
      dts += duration;
      if (frameDurationOut && !*frameDurationOut && duration) *frameDurationOut = ticksToNs(duration, timescale);
   }
   // TEMP diag: only when a fragment parses to zero samples (the "no playable fragments" failure). silent
   // on success; on failure it dumps the moof + parse state so a recurrence is diagnosed without another
   // instrument-and-rebuild round. remove once the sports-video class is confirmed fixed on hardware.
   if (count == 0) {
      char hex[2 * 128 + 1];
      uint64_t hexBytes = moofSize < 128 ? moofSize : 128;
      const char *digits = "0123456789abcdef";
      for (uint64_t i = 0; i < hexBytes; i++) { hex[2 * i] = digits[moof[i] >> 4]; hex[2 * i + 1] = digits[moof[i] & 15]; }
      hex[2 * hexBytes] = 0;
      logInfo("[demux-mp4] diag 0 samples moofSize=%llu trunFlags=0x%x sampleCount=%u trunEnd=%u moof=%s\n",
              (unsigned long long)moofSize, trunFlags, sampleCount, trunEnd, hex);
   }
   free(moof);

   // the next fragment starts after this fragment's mdat
   uint64_t mdatPos = moofStart + moofSize, mdatSize = 0, mdatHeader = 0;
   uint32_t mdatType;
   if (readMp4Box(source, mdatPos, &mdatType, &mdatSize, &mdatHeader) == 0 && mdatSize > 0)
      demuxer->nextMoofPos = mdatPos + mdatSize;
   else
      demuxer->nextMoofPos = demuxer->fileEnd ? demuxer->fileEnd : ~(uint64_t)0;   // no trailing mdat: stop (past-EOF sentinel when size unknown)

   return (int)count;
}

// refills the primary track's current fragment: audio in audio-only mode, otherwise video.
static int loadNextFragment(Mp4Demuxer *demuxer)
{
   if (demuxer->audioOnly) {
      demuxer->audioSampleCount = loadFragmentInto(demuxer, demuxer->audioSamples, MAX_FRAG_SAMPLES, demuxer->audioTimescale, 0);
      demuxer->audioCursor = 0;
      return demuxer->audioSampleCount > 0;
   }
   demuxer->videoSampleCount = loadFragmentInto(demuxer, demuxer->videoSamples, MAX_FRAG_SAMPLES, demuxer->videoTimescale, &demuxer->frameDurationNs);
   demuxer->videoCursor = 0;
   return demuxer->videoSampleCount > 0;
}

int openMp4Demuxer(Mp4Demuxer *demuxer, VideoSource *source, int audioOnly)
{
   memset(demuxer, 0, sizeof *demuxer);
   demuxer->audioOnly = audioOnly;
   demuxer->source = *source;            // adopt the already-open source (caller opened + sniffed it)
   memset(source, 0, sizeof *source);    // ownership moved here; caller must not close it

   // section: find moov among the top-level boxes (it may trail the media data). for a fragmented
   // file the first moof is right after moov (and the init segment), so stop once both are found
   // rather than walking every fragment (which would seek across the whole file).
   // fileEnd is the Content-Length; some googlevideo variants send none (fileEnd == 0 = unknown). the
   // body still streams fine, so treat unknown size as "scan until a read hits EOF" rather than as empty
   // - readMp4Box returns -1 past the true end (http-fs yields 0 bytes there), which breaks the loop.
   uint64_t fileEnd = getVideoSourceSize(&demuxer->source);
   uint64_t moovStart = 0, moovEnd = 0, firstMoofPos = 0, sidxPos = 0, sidxSize = 0, pos = 0;
   int fragmented = 0;
   int guard = 0;
   while ((fileEnd == 0 || pos + 8 <= fileEnd) && guard++ < 256) {
      uint32_t type; uint64_t size, headerLen;
      if (readMp4Box(&demuxer->source, pos, &type, &size, &headerLen) != 0 || size == 0) break;
      if (type == FOURCC('m','o','o','v')) { moovStart = pos + headerLen; moovEnd = pos + size; }
      if (type == FOURCC('s','i','d','x')) { sidxPos = pos; sidxSize = size; }   // seek index (before the first moof)
      if (type == FOURCC('m','o','o','f') && !firstMoofPos) { firstMoofPos = pos; fragmented = 1; }
      if (moovStart && firstMoofPos) break;
      pos += size;
   }
   if (!moovStart) {
      // dump the stream start so we can tell WHAT this actually is (an HTML/redirect error page, a
      // different mp4 layout, a SABR body...) - this fires on the "20 formats + itag 140" video variant
      uint8_t head[16] = {0};
      readVideoSourceAt(&demuxer->source, 0, head, sizeof head);
      logError("[demux-mp4] no moov box found (size=%llu first16 %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x)\n",
               (unsigned long long)fileEnd, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7],
               head[8], head[9], head[10], head[11], head[12], head[13], head[14], head[15]);
      goto fail;
   }

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

   // the primary track is video, or the mp4a track when opened audio-only (a DASH audio-only stream)
   if (audioOnly) { if (!demuxer->hasAudio) { logError("[demux-mp4] no mp4a audio track\n"); goto fail; } }
   else           { if (!demuxer->h264.valid) { logError("[demux-mp4] no decodable H.264 track\n"); goto fail; } }

   // fragmented: moov gave us the codec config but no samples; take the primary track's mvex/trex
   // defaults and prime the first fragment so the rest of open (and the player) sees a ready run.
   if (fragmented) {
      uint64_t mvexStart, mvexEnd, trexStart, trexEnd;
      if (findMp4ChildBox(&demuxer->source, moovStart, moovEnd, FOURCC('m','v','e','x'), &mvexStart, &mvexEnd) == 0 &&
          findMp4ChildBox(&demuxer->source, mvexStart, mvexEnd, FOURCC('t','r','e','x'), &trexStart, &trexEnd) == 0) {
         uint8_t trex[24];
         if (readVideoSourceAt(&demuxer->source, trexStart, trex, sizeof trex) == 0) {
            demuxer->trexDuration = readU32BE(trex + 12);
            demuxer->trexSize     = readU32BE(trex + 16);
            demuxer->trexFlags    = readU32BE(trex + 20);
         }
      }
      Mp4Sample **primarySamples = audioOnly ? &demuxer->audioSamples : &demuxer->videoSamples;
      *primarySamples = (Mp4Sample *)malloc(MAX_FRAG_SAMPLES * sizeof(Mp4Sample));
      if (!*primarySamples) goto fail;
      demuxer->fragmented   = 1;
      demuxer->fileEnd      = fileEnd;
      demuxer->firstMoofPos = firstMoofPos;
      demuxer->nextMoofPos  = firstMoofPos;
      demuxer->sidxPos      = sidxPos;
      demuxer->sidxSize     = sidxSize;
      if (!loadNextFragment(demuxer)) { logError("[demux-mp4] no playable fragments\n"); goto fail; }
      logInfo("[demux-mp4] fragmented stream, %d samples in first fragment\n",
              audioOnly ? demuxer->audioSampleCount : demuxer->videoSampleCount);
   }

   int primaryCount = audioOnly ? demuxer->audioSampleCount : demuxer->videoSampleCount;
   if (!primaryCount) { logError("[demux-mp4] no playable samples\n"); goto fail; }
   if (!demuxer->fragmented && !demuxer->durationNs)
      demuxer->durationNs = audioOnly ? demuxer->audioSamples[primaryCount - 1].ptsNs
                                      : demuxer->videoSamples[primaryCount - 1].ptsNs + demuxer->frameDurationNs;

   // audio-only reads samples straight into the AudioAu; it needs no Annex-B buffers or audio queue
   if (!audioOnly) {
      for (int i = 0; i < AU_BUFFER_COUNT; i++) {
         demuxer->auBuffers[i] = (uint8_t *)malloc(AU_BUFFER_SIZE);
         if (!demuxer->auBuffers[i]) goto fail;
      }
      demuxer->sampleBuffer = (uint8_t *)malloc(SAMPLE_BUFFER_SIZE);
      if (!demuxer->sampleBuffer) goto fail;
      demuxer->auCapacity     = AU_BUFFER_SIZE;
      demuxer->sampleCapacity = SAMPLE_BUFFER_SIZE;
      if (demuxer->hasAudio && createAudioAuQueue(&demuxer->audioQueue) != 0) demuxer->hasAudio = 0;   // no queue: play silent rather than fail
   }
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
   for (;;) {
      if (demuxer->videoCursor >= demuxer->videoSampleCount) {
         if (!demuxer->fragmented || !loadNextFragment(demuxer)) break;   // plain: real EOS. fragmented: no more fragments
      }
      const Mp4Sample *sample = &demuxer->videoSamples[demuxer->videoCursor];
      pumpAudioSamples(demuxer, sample->ptsNs);
      demuxer->videoCursor++;

      if (sample->size == 0 || sample->size > (uint32_t)demuxer->sampleCapacity) continue;   // corrupt entry: skip
      if (readVideoSourceAt(&demuxer->source, sample->offset, demuxer->sampleBuffer, sample->size) != 0) return 0;   // truncated: end of stream

      uint8_t *auBuffer = demuxer->auBuffers[demuxer->auIndex];   // alternate so the previous AU stays valid in the decoder
      if (!buildVideoAu(&demuxer->h264, demuxer->sampleBuffer, (int)sample->size, sample->keyframe, auBuffer, demuxer->auCapacity, sample->ptsNs, au)) continue;
      demuxer->auIndex = (demuxer->auIndex + 1) % AU_BUFFER_COUNT;
      return 1;
   }
   pumpAudioSamples(demuxer, demuxer->durationNs);   // video done: let the audio tail through
   return 0;
}

// audio-only: serve the next AAC frame straight into the AudioAu (no queue, no Annex-B). The audio
// thread copies it out immediately, so reading into au->data is safe. 1 = got a frame, 0 = end.
int readMp4AudioAu(Mp4Demuxer *demuxer, AudioAu *au)
{
   for (;;) {
      if (demuxer->audioCursor >= demuxer->audioSampleCount) {
         if (!demuxer->fragmented || !loadNextFragment(demuxer)) return 0;
      }
      const Mp4Sample *sample = &demuxer->audioSamples[demuxer->audioCursor++];
      if (sample->size == 0 || sample->size > AUDIO_AU_MAX_BYTES) {
         if (!demuxer->oversizeAudioWarned) { logWarn("[demux-mp4] oversized audio sample skipped\n"); demuxer->oversizeAudioWarned = 1; }
         continue;
      }
      if (readVideoSourceAt(&demuxer->source, sample->offset, au->data, sample->size) != 0) return 0;   // truncated: end
      au->size = (int)sample->size;
      au->pts  = sample->ptsNs;
      return 1;
   }
}

// ============================================================================
// seek
// ============================================================================

// find the fragment covering targetNs via the sidx segment index. the sidx (present in DASH/googlevideo
// streams, right after moov) lists every subsegment's byte size and duration, so the target fragment's
// offset comes from one small read + an in-memory scan - no per-fragment network I/O. sets nextMoofPos to
// that fragment's start and returns its start time. returns 0 (restart from the first fragment) when there
// is no usable index, so a seek never falls back to reading the whole stream.
static uint64_t seekViaSidx(Mp4Demuxer *demuxer, uint64_t targetNs, int landAfter)
{
   demuxer->nextMoofPos = demuxer->firstMoofPos;
   if (demuxer->sidxSize < 32 || demuxer->sidxSize > MAX_SIDX_BYTES) {
      logWarn("[demux-mp4] seek: no sidx index, restarting from the first fragment\n");
      return 0;
   }

   uint8_t *sidx = (uint8_t *)malloc((size_t)demuxer->sidxSize);
   if (!sidx) return 0;
   if (readVideoSourceAt(&demuxer->source, demuxer->sidxPos, sidx, demuxer->sidxSize) != 0) {
      free(sidx);
      logWarn("[demux-mp4] seek: sidx read failed, restarting from the first fragment\n");
      return 0;
   }

   // sidx layout: box header (8) + FullBox version/flags (4) + reference_ID (4) + timescale (4), then
   // earliest_presentation_time + first_offset (4 or 8 bytes each by version), reserved (2), reference_count (2)
   // a version-1 sidx uses 64-bit time/offset fields, so its header runs to byte 40; the >=32 floor
   // above only covers version 0. reject a short v1 box rather than reading past the buffer.
   uint8_t version = sidx[8];
   if (version != 0 && demuxer->sidxSize < 40) { free(sidx); logWarn("[demux-mp4] seek: sidx too small for v1\n"); return 0; }
   uint32_t timescale = readU32BE(sidx + 16);
   if (timescale == 0) timescale = 1;
   uint32_t cursor = version == 0 ? 20 + 8 : 20 + 16;   // skip earliest_presentation_time + first_offset
   uint64_t firstOffset = version == 0 ? readU32BE(sidx + 24) : readU64BE(sidx + 28);
   uint32_t referenceCount = readU16BE(sidx + cursor + 2);
   cursor += 4;

   // each reference entry (12 bytes): [reference_type:1][referenced_size:31], subsegment_duration, SAP info.
   // accumulate byte offset + time across the entries. normal seek lands on the subsegment covering the target
   // (the last one starting at/before it); landAfter lands on the first subsegment starting at/after the target
   // (used to skip fully past a segment - the video resumes just after it instead of replaying its tail).
   uint64_t offset = demuxer->sidxPos + demuxer->sidxSize + firstOffset;   // first subsegment (moof) start
   uint64_t timeTicks = 0, landOffset = offset, landTicks = 0;
   for (uint32_t i = 0; i < referenceCount && cursor + 12 <= demuxer->sidxSize; i++, cursor += 12) {
      uint32_t sizeField = readU32BE(sidx + cursor);
      uint32_t duration  = readU32BE(sidx + cursor + 4);
      uint64_t startNs = ticksToNs(timeTicks, timescale);
      if (landAfter) {
         landOffset = offset;
         landTicks  = timeTicks;
         if (startNs >= targetNs) break;   // first subsegment at/after the target
      } else {
         if (startNs > targetNs) break;    // this subsegment begins past the target: the previous one covers it
         landOffset = offset;
         landTicks  = timeTicks;
      }
      offset    += sizeField & 0x7FFFFFFF;   // referenced_size (mask off the reference_type bit)
      timeTicks += duration;
   }
   free(sidx);

   demuxer->nextMoofPos = landOffset;
   uint64_t landedNs = ticksToNs(landTicks, timescale);
   logInfo("[demux-mp4] %s sidx seek: %u refs, target %us -> %us\n", demuxer->audioOnly ? "audio" : "video",
           referenceCount, (unsigned)(targetNs / 1000000000ull), (unsigned)(landedNs / 1000000000ull));
   return landedNs;
}

// audio frames all decode independently, so a seek lands audio on the first sample at or after the target
// (video must instead start on a keyframe). positions audioCursor there and returns its pts, or fallbackNs
// when the target is past the last loaded sample. Leaving audio at the fragment start (before the target)
// would pin the audio-mastered A/V clock below the video frames, so none ever come due (blank screen).
static uint64_t seekAudioToTarget(Mp4Demuxer *demuxer, uint64_t targetNs, uint64_t fallbackNs)
{
   demuxer->audioCursor = 0;
   while (demuxer->audioCursor < demuxer->audioSampleCount && demuxer->audioSamples[demuxer->audioCursor].ptsNs < targetNs)
      demuxer->audioCursor++;
   return demuxer->audioCursor < demuxer->audioSampleCount ? demuxer->audioSamples[demuxer->audioCursor].ptsNs : fallbackNs;
}

uint64_t seekMp4Demuxer(Mp4Demuxer *demuxer, uint64_t targetNs, int landAfter)
{
   clearAudioAuQueue(&demuxer->audioQueue);   // consumer is parked: discard queued audio

   // fragmented: no sample table, so use the sidx index to jump straight to the covering fragment.
   if (demuxer->fragmented) {
      uint64_t landedNs = seekViaSidx(demuxer, targetNs, landAfter);
      loadNextFragment(demuxer);
      if (demuxer->audioOnly) return seekAudioToTarget(demuxer, targetNs, landedNs);
      // video: decoding must begin on the fragment's first sample (its keyframe), so report that time
      if (demuxer->videoSampleCount == 0) return landedNs;
      logInfo("[demux-mp4] seek fragment first sample %s\n", demuxer->videoSamples[0].keyframe ? "sync" : "NON-SYNC");
      return demuxer->videoSamples[0].ptsNs;
   }

   // audio-only plain: every sample is a sync sample; land on the first at or after the target
   if (demuxer->audioOnly) return seekAudioToTarget(demuxer, targetNs, targetNs);

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
   seekAudioToTarget(demuxer, landedNs, landedNs);   // restart audio at the first sample due at the landed position
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
