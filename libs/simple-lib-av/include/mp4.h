#pragma once

// mp4 - the shared ISOBMFF (MP4) box reader, the MP4 analogue of ebml.h. A box is a 32-bit size and
// a fourcc type followed by its payload; size 1 switches to a 64-bit size, size 0 runs to EOF.
// Shared by video-probe (track inspection) and demux-mp4 (full demux).

#include "video-source.h"

#define FOURCC(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

static inline uint32_t readU32BE(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static inline uint16_t readU16BE(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static inline uint64_t readU64BE(const uint8_t *p) { return ((uint64_t)readU32BE(p) << 32) | readU32BE(p + 4); }

// writes a fourcc as its four ASCII bytes into out[5] (NUL-terminated), for log/user messages.
void fourccToStr(uint32_t fourcc, char out[5]);

// reads a box header at absolute `off`. fills type + total box size + header length. 0 / -1.
int readMp4Box(VideoSource *source, uint64_t off, uint32_t *type, uint64_t *boxSize, uint64_t *headerLen);

// finds the first child box of `wantType` in [start, end). fills the child's payload range. 0 / -1.
int findMp4ChildBox(VideoSource *source, uint64_t start, uint64_t end, uint32_t wantType, uint64_t *payloadStart, uint64_t *payloadEnd);
