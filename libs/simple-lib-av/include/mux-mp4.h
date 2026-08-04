#pragma once

// mux-mp4 - a minimal MP4 (ISO base media) writer. Takes the elementary access units the demuxers
// produce (Annex-B H.264 + raw AAC frames) and writes them into one .mp4 that the PS3's own video
// player accepts.
//
// The samples go straight into an mdat box as they arrive; the sample tables are collected in memory
// and written as a moov box at the end, then the mdat size is patched. H.264 is stored as avc1 with an
// avcC record built from the demuxer's SPS/PPS and each access unit rewritten from Annex-B to
// length-prefixed NAL units. AAC is stored as mp4a with an esds descriptor.

#include <stdint.h>
#include "vfs.h"
#include "h264.h"

// per-sample tables collected while writing, turned into stts/ctts/stss/stsz/stco at close.
typedef struct {
   uint64_t *offsets;      // file offset of each sample
   uint32_t *sizes;
   uint32_t *times;        // presentation time in the track's timescale
   uint32_t *syncSamples;  // 1-based indices of the keyframes (video track only)
   int       count, capacity;
   int       syncCount, syncCapacity;
   int       outOfMemory;
} Mp4Track;

typedef struct {
   VfsFile  file;
   int      isOpen;
   int      hasAudio;
   uint64_t bytesWritten;      // total bytes committed to the file (progress / diagnostics)

   Mp4Track video, audio;
   uint64_t mdatStart;         // file offset of the mdat box header, patched with the real size at close

   uint8_t *sample;            // scratch for one length-prefixed video access unit
   int      sampleCapacity;

   uint8_t  avcc[640];
   int      avccLength;
   int      width, height;
   int      audioRate, audioChannels;
   uint32_t frameDurationTicks;   // video timescale ticks per frame (0 if the source didn't say)
} Mp4Muxer;

// begin an .mp4 at path. h264 / width / height / frameDurationNs describe the video track; the stream's
// duration is not needed, it is measured from the samples and written at close. pass audioRate /
// audioChannels > 0 to add an AAC track, or 0 for video-only. returns 0 on success, -1 on failure
// (leaves nothing open, removes a partially written file).
int openMp4Muxer(Mp4Muxer *mux, const char *path, const H264Config *h264, int width, int height,
                 uint64_t frameDurationNs, int audioRate, int audioChannels);

// append one video access unit (Annex-B) presenting at ptsNs; keyframe = 1 for an IDR. 0 / -1.
int writeMp4Video(Mp4Muxer *mux, const uint8_t *annexB, int size, uint64_t ptsNs, int keyframe);

// append one raw AAC frame presenting at ptsNs. 0 / -1.
int writeMp4Audio(Mp4Muxer *mux, const uint8_t *aac, int size, uint64_t ptsNs);

// write the moov box, patch the mdat size and close the file. 0 / -1.
int closeMp4Muxer(Mp4Muxer *mux);
