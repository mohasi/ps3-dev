#pragma once

// mux-mkv - a minimal streaming Matroska (MKV) writer, the reverse of demux-mkv. Takes the elementary
// access units the demuxers produce (Annex-B H.264 + raw AAC frames) and writes them into one .mkv
// without ever seeking back over the file, so a live download can be remuxed straight to disk.
//
// H.264 is stored as V_MPEG4/ISO/AVC: an avcC record built from the demuxer's SPS/PPS, with each access
// unit converted from Annex-B to length-prefixed NAL units. AAC is stored as A_AAC with a 2-byte
// AudioSpecificConfig derived from the sample rate + channel count. Element ids come from ebml.h.

#include <stdint.h>
#include "vfs.h"
#include "h264.h"

typedef struct {
   VfsFile  file;
   int      isOpen;
   int      hasAudio;
   uint64_t bytesWritten;      // total bytes committed to the file (progress / diagnostics)

   // the in-progress cluster is buffered so its size is known when it's flushed (the file is written
   // strictly forward - no seeking back to patch a size).
   uint8_t *cluster;
   int      clusterLen;
   int      clusterCap;
   uint64_t clusterBaseMs;     // timecode the open cluster started at (UINT64_MAX = no cluster open)
} MkvMuxer;

// begin an .mkv at path. h264 / width / height / frameDurationNs describe the video track; durationNs is
// the whole-stream length for the Info header (0 = unknown). pass audioRate / audioChannels > 0 to add an
// AAC track, or 0 for video-only. returns 0 on success, -1 on failure (leaves nothing open, removes a
// partially written file).
int openMkvMuxer(MkvMuxer *mux, const char *path, const H264Config *h264, int width, int height,
                 uint64_t frameDurationNs, uint64_t durationNs, int audioRate, int audioChannels);

// append one video access unit (Annex-B) presenting at ptsNs; keyframe = 1 for an IDR. 0 / -1.
int writeMkvVideo(MkvMuxer *mux, const uint8_t *annexB, int size, uint64_t ptsNs, int keyframe);

// append one raw AAC frame presenting at ptsNs. 0 / -1.
int writeMkvAudio(MkvMuxer *mux, const uint8_t *aac, int size, uint64_t ptsNs);

// flush the last cluster and close the file. 0 / -1.
int closeMkvMuxer(MkvMuxer *mux);
