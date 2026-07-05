#pragma once

// demux - the container-independent demuxer the player talks to. Opens MKV or MP4 by sniffing the
// file's magic bytes, mirrors the track metadata into one place, and dispatches reads and seeks to
// the container's demuxer. Both containers produce the same Annex-B video access units and queued
// AAC audio frames.

#include "demux-mkv.h"
#include "demux-mp4.h"

typedef struct {
   int isMp4;
   union { MkvDemuxer mkv; Mp4Demuxer mp4; } container;

   // track metadata mirrored from the container at open
   H264Config h264;             // SPS/PPS header + NAL length size + ref-frame count
   int        level;            // AVC level, passed to cellVdec as profileLevel
   int        width, height;
   int        hasAudio;         // an AAC track was found (anything else plays silent)
   int        audioRate;        // sampling frequency in Hz
   int        audioChannels;
   uint64_t   frameDurationNs;  // nanoseconds per video frame (0 if the file doesn't say)
   uint64_t   durationNs;       // total stream duration in nanoseconds (0 if unknown)
} VideoDemuxer;

int openVideoDemuxer(VideoDemuxer *demuxer, const char *path);   // 0 on success (decodable video track found), -1 otherwise

static inline int readVideoAu(VideoDemuxer *demuxer, VideoAu *au)   // 1 = got AU, 0 = end of stream, -1 = error
{
   return demuxer->isMp4 ? readMp4VideoAu(&demuxer->container.mp4, au) : readMkvVideoAu(&demuxer->container.mkv, au);
}

static inline int takeAudioAu(VideoDemuxer *demuxer, AudioAu *au)   // pops a queued audio AU: 1 = got one, 0 = queue empty
{
   return takeQueuedAudioAu(demuxer->isMp4 ? &demuxer->container.mp4.audioQueue : &demuxer->container.mkv.audioQueue, au);
}

// jumps to the last keyframe at or before targetNs and clears the audio queue (only call with the
// audio consumer parked). Returns the time actually landed on.
static inline uint64_t seekVideoDemuxer(VideoDemuxer *demuxer, uint64_t targetNs)
{
   return demuxer->isMp4 ? seekMp4Demuxer(&demuxer->container.mp4, targetNs) : seekMkvDemuxer(&demuxer->container.mkv, targetNs);
}

static inline void closeVideoDemuxer(VideoDemuxer *demuxer)
{
   if (demuxer->isMp4) closeMp4Demuxer(&demuxer->container.mp4);
   else closeMkvDemuxer(&demuxer->container.mkv);
}
