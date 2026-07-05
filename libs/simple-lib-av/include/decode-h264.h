#pragma once

// decode-h264 - a thin wrapper over cellVdec (the PS3's hardware H.264 decoder, run across 4 SPUs).
// Feed it Annex-B access units; pull decoded frames back as YUV 4:2:0 planar (Y then U then V,
// packed, width*height*3/2 bytes). Colour conversion happens on the RSX at draw time — asking the
// decoder for RGB makes it convert on the same SPUs that decode, which costs real throughput.

#include <stdint.h>

typedef struct H264Decoder H264Decoder;

// brings up a decoder for a stream of the given coded size and AVC level (AVCLevelIndication).
// maxRefFrames is the SPS reference-frame count; the decoder asks cellVdec for that many decoded-frame
// buffers (+1) so high-reference streams don't overflow the level's default pool. NULL on failure.
H264Decoder *createH264Decoder(int width, int height, int level, int maxRefFrames);

// feeds one Annex-B access unit without waiting for it to be consumed. pts is opaque (passed through
// to the decoded picture). returns 0 when fed, 1 when the decoder's queues are full (pull pictures
// with getFrameH264, then retry the same AU), -1 on error. `data` must stay untouched until the
// backlog says the decoder has consumed it.
int decodeAuH264(H264Decoder *decoder, const uint8_t *data, int size, uint64_t pts);

// access units fed but not yet consumed. A caller with N rotating AU buffers may safely demux a
// new AU while the backlog is below N.
int getAuBacklogH264(const H264Decoder *decoder);

// retrieves a decoded frame as YUV 4:2:0 planar into `yuvOut` (must hold width*height*3/2 bytes,
// 128-byte aligned, size a multiple of 128). frames come out in presentation order. returns 1 and
// sets *outWidth/*outHeight/*outPts (the pts passed in for that frame's AU) when a frame was ready,
// 0 if none pending, -1 on error.
int getFrameH264(H264Decoder *decoder, void *yuvOut, int *outWidth, int *outHeight, uint64_t *outPts);

// flushes the decode pipeline for a seek: ends the sequence (discarding in-flight pictures) and
// starts a fresh one on the same handle. Feed the new position's keyframe next. 0 ok, -1 on error.
int resetH264Decoder(H264Decoder *decoder);

void destroyH264Decoder(H264Decoder *decoder);
