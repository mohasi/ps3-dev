#pragma once

// h264 - the bitstream reshaping cellVdec needs. MKV/MP4 store H.264 as an avcC record (SPS/PPS out
// of band) with each sample as length-prefixed NAL units. cellVdec wants Annex-B: NAL units with
// 00 00 00 01 start codes, and the SPS/PPS prepended to each IDR access unit. This module extracts
// the SPS/PPS as an Annex-B header from avcC and converts length-prefixed samples to Annex-B.

#include <stdint.h>

typedef struct {
   uint8_t header[512];    // SPS + PPS as Annex-B, prepended before each IDR access unit
   int     headerSize;
   int     nalLengthSize;  // 1..4: bytes of each NAL's length prefix in a sample
   int     maxRefFrames;   // SPS max_num_ref_frames; sizes the decoder's frame-buffer pool (DPB)
   int     refFramesExceedDpb;   // 1 when the SPS wants more ref frames than the DPB holds and can't be patched down
   int     valid;
} H264Config;

// parses an avcC (AVCDecoderConfigurationRecord) into the Annex-B SPS/PPS header + NAL length size.
// returns 0 on success, -1 if malformed or the header doesn't fit.
int parseAvcc(const uint8_t *avcc, int size, H264Config *config);

// converts one length-prefixed (AVCC) sample into Annex-B start-code form in `out`. returns bytes
// written, or -1 if malformed / the output doesn't fit. *isIdr is set when the sample carries an IDR
// slice (NAL type 5), i.e. a keyframe that must be preceded by the SPS/PPS header.
int avccToAnnexB(const uint8_t *in, int inSize, int nalLengthSize, uint8_t *out, int outCap, int *isIdr);

typedef struct {
   int codedWidth;    // what the SPS actually codes - see the warning below
   int codedHeight;
   int level;         // level_idc, as cellVdec wants it
   int maxRefFrames;  // SPS max_num_ref_frames
} H264StreamInfo;

// reads a stream's own description out of the SPS in an Annex-B buffer (typically its first
// keyframe). returns 0 on success, -1 if there is no parsable SPS.
//
// Use this to configure the decoder rather than trusting a size from elsewhere. The CODED size is
// not the display size: H.264 codes whole 16x16 blocks and hardware encoders pad further (Intel
// QuickSync codes 1280x720 as 1280x736, cropping it back on display). cellVdec must be given the
// coded size exactly - told less it silently decodes nothing into an untouched buffer (a black
// screen with no error), told more it locks the console.
int readH264StreamInfo(const uint8_t *annexB, int size, H264StreamInfo *info);

// rewrites the SPS's max_num_ref_frames down to what the PS3 decoder's DPB can hold at the stream's
// frame size (level 4.2 budget), in place. returns the capped count, 0 if unchanged, -1 if a cap was
// needed but the bit patch is impossible. `sps` starts at the NAL header byte.
int patchSpsToDpbCap(uint8_t *sps, int size);
