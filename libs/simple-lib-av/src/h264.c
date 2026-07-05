// h264 - avcC parsing + AVCC-to-Annex-B conversion for cellVdec. See h264.h.
#include "h264.h"
#include "dbg.h"
#include <string.h>

static const uint8_t START_CODE[4] = { 0, 0, 0, 1 };

// minimal SPS bit reader: enough to reach max_num_ref_frames. cellVdec sizes its decoded-frame pool
// (the DPB) from the level, but a stream can legally ask for more reference frames than its declared
// level's DPB holds (common in BDrip encodes). Reading the real count lets us size the pool to fit,
// otherwise the decoder conceals every inter-frame as black.

typedef struct { const uint8_t *data; int size; int bitPos; } BitReader;

static int readBit(BitReader *reader)
{
   int byteIndex = reader->bitPos >> 3;
   if (byteIndex >= reader->size) return 0;
   int bit = (reader->data[byteIndex] >> (7 - (reader->bitPos & 7))) & 1;
   reader->bitPos++;
   return bit;
}

static uint32_t readBits(BitReader *reader, int count)
{
   uint32_t value = 0;
   while (count-- > 0) value = (value << 1) | readBit(reader);
   return value;
}

static uint32_t readUe(BitReader *reader)   // Exp-Golomb unsigned
{
   int zeros = 0;
   while (readBit(reader) == 0 && zeros < 32) zeros++;
   return (uint32_t)((1u << zeros) - 1 + (zeros ? readBits(reader, zeros) : 0));
}

static int readSe(BitReader *reader)        // Exp-Golomb signed
{
   uint32_t k = readUe(reader);
   return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

// strips H.264 emulation-prevention bytes (00 00 03 -> 00 00) into `out`; returns bytes written.
// `sourceIndex` (optional) records, per output byte, its index in `in` so patched bytes can be
// written back to the emulation-encoded original.
static int stripEmulation(const uint8_t *in, int inSize, uint8_t *out, int *sourceIndex, int outCap)
{
   int outSize = 0;
   for (int i = 0; i < inSize && outSize < outCap; i++) {
      if (i >= 2 && in[i] == 3 && in[i - 1] == 0 && in[i - 2] == 0) continue;
      if (sourceIndex) sourceIndex[outSize] = i;
      out[outSize++] = in[i];
   }
   return outSize;
}

typedef struct {
   int refFrames;      // max_num_ref_frames
   int refFramesBit;   // rbsp bit position where its Exp-Golomb code starts
   int frameMbs;       // coded frame size in macroblocks
} SpsFields;

// walks the stripped SPS rbsp up to the frame dimensions, filling `fields`. returns 0 / -1.
static int walkSps(const uint8_t *rbsp, int rbspSize, SpsFields *fields)
{
   BitReader reader = { rbsp, rbspSize, 0 };
   int profile = readBits(&reader, 8);
   readBits(&reader, 8);                 // constraint flags + reserved
   readBits(&reader, 8);                 // level_idc
   readUe(&reader);                      // seq_parameter_set_id

   int isHighProfile = profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
                        profile == 44  || profile == 83  || profile == 86  || profile == 118 ||
                        profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135;
   if (isHighProfile) {
      int chroma = readUe(&reader);
      if (chroma == 3) readBit(&reader);   // separate_colour_plane_flag
      readUe(&reader);                     // bit_depth_luma_minus8
      readUe(&reader);                     // bit_depth_chroma_minus8
      readBit(&reader);                    // qpprime_y_zero_transform_bypass_flag
      if (readBit(&reader)) {              // seq_scaling_matrix_present_flag
         int listCount = chroma == 3 ? 12 : 8;
         for (int list = 0; list < listCount; list++) {
            if (!readBit(&reader)) continue;   // scaling_list_present_flag
            int listSize = list < 6 ? 16 : 64, lastScale = 8, nextScale = 8;
            for (int j = 0; j < listSize; j++) {
               if (nextScale != 0) nextScale = (lastScale + readSe(&reader) + 256) % 256;
               lastScale = nextScale ? nextScale : lastScale;
            }
         }
      }
   }

   readUe(&reader);                      // log2_max_frame_num_minus4
   int pocType = readUe(&reader);
   if (pocType == 0) {
      readUe(&reader);                   // log2_max_pic_order_cnt_lsb_minus4
   } else if (pocType == 1) {
      readBit(&reader);                  // delta_pic_order_always_zero_flag
      readSe(&reader); readSe(&reader);  // offsets
      int cycle = readUe(&reader);
      for (int i = 0; i < cycle; i++) readSe(&reader);
   }

   fields->refFramesBit = reader.bitPos;
   fields->refFrames    = (int)readUe(&reader);
   readBit(&reader);                     // gaps_in_frame_num_value_allowed_flag
   int widthMbs   = (int)readUe(&reader) + 1;   // pic_width_in_mbs_minus1
   int heightUnits = (int)readUe(&reader) + 1;  // pic_height_in_map_units_minus1
   int frameMbsOnly = readBit(&reader);
   fields->frameMbs = widthMbs * heightUnits * (frameMbsOnly ? 1 : 2);
   return reader.bitPos <= rbspSize * 8 ? 0 : -1;
}

// writes `count` bits of `value` (MSB first) into `rbsp` at `bitPos`, mirroring each touched byte
// back into the emulation-encoded `raw` via `sourceIndex`.
static void writeBitsPatched(uint8_t *rbsp, uint8_t *raw, const int *sourceIndex, int bitPos, uint32_t value, int count)
{
   for (int i = 0; i < count; i++) {
      int bit = (value >> (count - 1 - i)) & 1;
      int byteIndex = (bitPos + i) >> 3, shift = 7 - ((bitPos + i) & 7);
      rbsp[byteIndex] = (uint8_t)((rbsp[byteIndex] & ~(1 << shift)) | (bit << shift));
      raw[sourceIndex[byteIndex]] = rbsp[byteIndex];
   }
}

// Exp-Golomb code length of ue(value): 2 * bits(value + 1) - 1
static int bitLengthUe(int value) { int topBits = 0; for (int v = value + 1; v; v >>= 1) topBits++; return 2 * topBits - 1; }

// PS3 cellVdec tops out at AVC level 4.2, whose DPB budget is 34816 macroblocks (H.264 Table A-1).
// A stream may declare more reference frames than fit (common in BDrip x264 --ref 5 encodes); the
// hardware then rejects the whole sequence and every picture conceals to black (ERROR_PIC). This
// rewrites max_num_ref_frames in the SPS down to what the DPB actually holds — an in-place bit patch
// when the old and new Exp-Golomb codes are the same length (covers the 5..6 -> 4 case at 1080p).
// `sps` starts at the NAL header byte. returns the capped count, 0 if no patch was needed, -1 if a
// patch was needed but impossible.
int patchSpsToDpbCap(uint8_t *sps, int size)
{
   // parse the fields that size the DPB
   if (size < 4) return 0;
   uint8_t rbsp[96]; int sourceIndex[96];
   int rbspSize = stripEmulation(sps + 1, size - 1, rbsp, sourceIndex, sizeof rbsp);
   SpsFields fields;
   if (walkSps(rbsp, rbspSize, &fields) != 0 || fields.frameMbs <= 0) return 0;

   int dpbFrames = 34816 / fields.frameMbs;
   if (dpbFrames > 16) dpbFrames = 16;
   if (dpbFrames < 1)  dpbFrames = 1;
   if (fields.refFrames <= dpbFrames) return 0;
   if (bitLengthUe(fields.refFrames) != bitLengthUe(dpbFrames)) return -1;   // would shift the rest of the SPS

   // patch, then verify by re-reading the patched bytes (also catches a disturbed emulation pattern)
   writeBitsPatched(rbsp, sps + 1, sourceIndex, fields.refFramesBit, (uint32_t)(dpbFrames + 1), bitLengthUe(dpbFrames));
   uint8_t check[96];
   int checkSize = stripEmulation(sps + 1, size - 1, check, 0, sizeof check);
   SpsFields patched;
   if (checkSize != rbspSize || walkSps(check, checkSize, &patched) != 0 || patched.refFrames != dpbFrames) return -1;
   return dpbFrames;
}

// returns the SPS's max_num_ref_frames, or 0 if it can't be parsed. `sps` starts at the NAL header byte.
static int parseSpsMaxRefFrames(const uint8_t *sps, int size)
{
   if (size < 4) return 0;
   uint8_t rbsp[96];
   int rbspSize = stripEmulation(sps + 1, size - 1, rbsp, 0, sizeof rbsp);   // skip the NAL header byte
   SpsFields fields;
   return walkSps(rbsp, rbspSize, &fields) == 0 ? fields.refFrames : 0;
}

// appends a start code + `len` bytes of `nal` into config->header, bounds-checked. 0 / -1.
static int appendHeaderNal(H264Config *config, const uint8_t *nal, int len)
{
   if (config->headerSize + 4 + len > (int)sizeof config->header) return -1;
   memcpy(config->header + config->headerSize, START_CODE, 4);
   config->headerSize += 4;
   memcpy(config->header + config->headerSize, nal, len);
   config->headerSize += len;
   return 0;
}

int parseAvcc(const uint8_t *avcc, int size, H264Config *config)
{
   memset(config, 0, sizeof *config);
   if (size < 7 || avcc[0] != 1) return -1;   // configurationVersion must be 1

   config->nalLengthSize = (avcc[4] & 0x03) + 1;

   int pos = 5;
   int spsCount = avcc[pos++] & 0x1F;
   for (int i = 0; i < spsCount; i++) {
      if (pos + 2 > size) return -1;
      int len = (avcc[pos] << 8) | avcc[pos + 1];
      pos += 2;
      if (pos + len > size || appendHeaderNal(config, avcc + pos, len) != 0) return -1;
      uint8_t *headerSps = config->header + config->headerSize - len;
      int capped = patchSpsToDpbCap(headerSps, len);
      if (capped > 0) logWarn("[h264] SPS wants %d ref frames, over the PS3 DPB cap - patched down to %d\n", parseSpsMaxRefFrames(avcc + pos, len), capped);
      else if (capped < 0) {   // the probe rejects such files with a user-facing reason
         config->refFramesExceedDpb = 1;
         logWarn("[h264] SPS ref frames exceed the PS3 DPB cap and can't be patched in place\n");
      }
      if (i == 0) config->maxRefFrames = parseSpsMaxRefFrames(headerSps, len);
      pos += len;
   }

   if (pos >= size) return -1;
   int ppsCount = avcc[pos++];
   for (int i = 0; i < ppsCount; i++) {
      if (pos + 2 > size) return -1;
      int len = (avcc[pos] << 8) | avcc[pos + 1];
      pos += 2;
      if (pos + len > size || appendHeaderNal(config, avcc + pos, len) != 0) return -1;
      pos += len;
   }

   config->valid = (config->headerSize > 0);
   return config->valid ? 0 : -1;
}

int avccToAnnexB(const uint8_t *in, int inSize, int nalLengthSize, uint8_t *out, int outCap, int *isIdr)
{
   int inPos = 0, outPos = 0;
   *isIdr = 0;

   while (inPos + nalLengthSize <= inSize) {
      int nalLen = 0;
      for (int i = 0; i < nalLengthSize; i++) nalLen = (nalLen << 8) | in[inPos++];
      if (nalLen <= 0 || inPos + nalLen > inSize) break;   // trailing padding / malformed

      if (outPos + 4 + nalLen > outCap) return -1;
      memcpy(out + outPos, START_CODE, 4);
      outPos += 4;
      if ((in[inPos] & 0x1F) == 5) *isIdr = 1;   // NAL type 5 = IDR slice
      memcpy(out + outPos, in + inPos, nalLen);
      if ((in[inPos] & 0x1F) == 7) patchSpsToDpbCap(out + outPos, nalLen);   // cap in-band SPS ref frames too
      outPos += nalLen;
      inPos  += nalLen;
   }
   return outPos;
}
