#pragma once

// demux-mp4 - pulls H.264 video access units and AAC audio frames out of an MP4 (ISOBMFF) container.
// MP4 carries a complete sample index up front (the moov box's stbl tables), so the whole file
// layout is known at open: each track's tables are flattened into an in-memory sample array
// (offset, size, pts, keyframe). Reads walk the video array in decode order while queueing the
// audio samples that fall due, and seeking is a keyframe lookup in the array. Fragmented MP4
// (moof) is not supported.

#include "video-source.h"
#include "demux-types.h"

typedef struct {
   uint64_t offset;       // absolute file offset of the sample
   uint32_t size;
   uint32_t keyframe;     // video: sync sample per stss (1 for every sample when the file has no stss)
   uint64_t ptsNs;        // presentation time (decode time + composition offset)
} Mp4Sample;

typedef struct {
   VideoSource source;

   H264Config h264;             // SPS/PPS header + NAL length size
   int        level;            // AVC level (AVCLevelIndication), passed to cellVdec as profileLevel
   int        width, height;

   int      hasAudio;           // an AAC (mp4a) track was found (other codecs: video plays silent)
   int      audioRate;          // sampling frequency in Hz
   int      audioChannels;

   uint64_t frameDurationNs;    // nanoseconds per video frame (first stts delta; 0 if unknown)
   uint64_t durationNs;         // total stream duration in nanoseconds (0 if unknown)

   Mp4Sample *videoSamples;     // flattened sample tables, in decode order
   int        videoSampleCount, videoCursor;
   Mp4Sample *audioSamples;
   int        audioSampleCount, audioCursor;

   AudioAuQueue audioQueue;
   int          oversizeAudioWarned;

   uint8_t *auBuffers[AU_BUFFER_COUNT];   // rotating Annex-B scratch: earlier AUs stay valid in the decoder's queue while later ones are demuxed
   int      auIndex;            // buffer the next readMp4VideoAu writes into
   int      auCapacity;
   uint8_t *sampleBuffer;       // reused raw length-prefixed sample scratch (heap)
   int      sampleCapacity;
} Mp4Demuxer;

int  openMp4Demuxer(Mp4Demuxer *demuxer, const char *path);   // 0 on success (video track found), -1 otherwise
int  readMp4VideoAu(Mp4Demuxer *demuxer, VideoAu *au);        // 1 = got AU, 0 = end of stream, -1 = error
void closeMp4Demuxer(Mp4Demuxer *demuxer);

// jumps to the last keyframe presenting at or before targetNs and clears the audio queue (only call
// with the audio consumer parked). Returns the time actually landed on.
uint64_t seekMp4Demuxer(Mp4Demuxer *demuxer, uint64_t targetNs);
