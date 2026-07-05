#pragma once

// ebml - the low-level Matroska/EBML element reader shared by the probe and the demuxer. EBML is a
// nested tag-length-value format: each element is a variable-length ID, a variable-length size, then
// that many bytes of data. Element IDs used across the video code are collected here.

#include "video-source.h"

// EBML / Matroska element IDs (stored with their length-descriptor bits, i.e. canonical form).
#define EBML_ID_HEADER         0x1A45DFA3u
#define EBML_ID_SEGMENT        0x18538067u
#define EBML_ID_INFO           0x1549A966u
#define EBML_ID_TIMECODESCALE  0x002AD7B1u
#define EBML_ID_DURATION       0x00004489u
#define EBML_ID_TRACKS         0x1654AE6Bu
#define EBML_ID_TRACKENTRY     0x000000AEu
#define EBML_ID_TRACKNUMBER    0x000000D7u
#define EBML_ID_TRACKTYPE      0x00000083u
#define EBML_ID_DEFAULTDURATION 0x0023E383u
#define EBML_ID_CODECID        0x00000086u
#define EBML_ID_CODECPRIVATE   0x000063A2u
#define EBML_ID_VIDEO          0x000000E0u
#define EBML_ID_PIXELWIDTH     0x000000B0u
#define EBML_ID_PIXELHEIGHT    0x000000BAu
#define EBML_ID_AUDIO          0x000000E1u
#define EBML_ID_SAMPLINGFREQ   0x000000B5u
#define EBML_ID_CHANNELS       0x0000009Fu
#define EBML_ID_SEEKHEAD       0x114D9B74u
#define EBML_ID_SEEK           0x00004DBBu
#define EBML_ID_SEEKID         0x000053ABu
#define EBML_ID_SEEKPOSITION   0x000053ACu
#define EBML_ID_CUES           0x1C53BB6Bu
#define EBML_ID_CUEPOINT       0x000000BBu
#define EBML_ID_CUETIME        0x000000B3u
#define EBML_ID_CUETRACKPOS    0x000000B7u
#define EBML_ID_CUECLUSTERPOS  0x000000F1u
#define EBML_ID_CLUSTER        0x1F43B675u
#define EBML_ID_TIMECODE       0x000000E7u
#define EBML_ID_SIMPLEBLOCK    0x000000A3u
#define EBML_ID_BLOCKGROUP     0x000000A0u
#define EBML_ID_BLOCK          0x000000A1u

// reads one EBML variable-length integer at the current source position. keepMarker=1 for element
// IDs (the length bits are part of the value); =0 for sizes (strip the marker to get the value).
// returns bytes consumed (1..8) or 0 on error. *unknown (may be NULL) is set when a size field is
// all-ones (a streamed / unknown size).
int readEbmlVint(VideoSource *source, uint64_t *value, int keepMarker, int *unknown);

// reads an element header (id + size) at the current position, leaving the source positioned at the
// element's data. returns 0 on success, -1 on error. *unknownSize (may be NULL) flags a streamed size.
int readEbmlElement(VideoSource *source, uint32_t *id, uint64_t *size, int *unknownSize);
