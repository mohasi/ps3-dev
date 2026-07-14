#pragma once

// demux-mp4 - pulls H.264 video access units and AAC audio frames out of an MP4 (ISOBMFF) container.
// Two layouts are handled:
//  - plain MP4: a complete sample index up front (the moov box's stbl tables), flattened into an
//    in-memory sample array (offset, size, pts, keyframe). Reads walk the array in decode order.
//  - fragmented MP4 (moof), as used by adaptive/DASH streams: moov carries only the codec config
//    plus mvex defaults; the per-sample info lives in moof/traf/trun boxes scattered through the
//    file. Indexing it all up front would mean reading the whole file, so instead one fragment is
//    parsed at a time and refilled as playback advances (streams linearly, no big seeks).

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
   int      audioRate;          // output sampling frequency in Hz (post-SBR for HE-AAC)
   int      audioAdtsRate;      // rate to write in the ADTS header: the core rate (= audioRate/2 for HE-AAC/SBR)
   int      audioChannels;

   uint64_t frameDurationNs;    // nanoseconds per video frame (first stts delta; 0 if unknown)
   uint64_t durationNs;         // total stream duration in nanoseconds (0 if unknown)

   Mp4Sample *videoSamples;     // plain: full table in decode order. fragmented: the current fragment's samples.
   int        videoSampleCount, videoCursor;
   Mp4Sample *audioSamples;
   int        audioSampleCount, audioCursor;

   // audio-only mode: the file has a single mp4a track and no video (a DASH audio-only stream, e.g.
   // YouTube itag 140). The audio track becomes the primary; readMp4AudioAu serves it directly and
   // its fragments load into audioSamples. In this mode the trex*/nextMoofPos state describes the
   // audio track, and there is no H.264 decoder / video pipeline.
   int      audioOnly;

   // fragmented (moof) streaming state
   int      fragmented;
   uint64_t fileEnd;              // total size, cached for the fragment walk
   uint64_t firstMoofPos;         // first fragment, for restart-on-seek
   uint64_t nextMoofPos;          // where the next fragment begins (end of the current mdat)
   uint64_t sidxPos, sidxSize;    // segment index box (DASH/googlevideo): maps time -> fragment offset for seeks (0 = none)
   uint8_t *sidxCache;            // the sidx box bytes, read once (it's immutable) so a seek costs no index re-fetch
   uint64_t videoTimescale;       // video track timescale (fragmented converts pts at read time)
   uint64_t audioTimescale;       // audio track timescale (audio-only fragmented converts pts at read time)
   uint32_t trexDuration, trexSize, trexFlags;   // mvex/trex per-sample defaults (of the primary track)

   AudioAuQueue audioQueue;
   int          oversizeAudioWarned;

   uint8_t *auBuffers[AU_BUFFER_COUNT];   // rotating Annex-B scratch: earlier AUs stay valid in the decoder's queue while later ones are demuxed
   int      auIndex;            // buffer the next readMp4VideoAu writes into
   int      auCapacity;
   uint8_t *sampleBuffer;       // reused raw length-prefixed sample scratch (heap)
   int      sampleCapacity;
} Mp4Demuxer;

// adopts an already-open `source` (the caller opens it and sniffs the container, so http streams
// aren't opened twice). audioOnly=0 opens a video file (audio, if any, is queued alongside);
// audioOnly=1 opens a standalone mp4a track (no video), read with readMp4AudioAu. 0 / -1.
int  openMp4Demuxer(Mp4Demuxer *demuxer, VideoSource *source, int audioOnly);
int  readMp4VideoAu(Mp4Demuxer *demuxer, VideoAu *au);        // 1 = got AU, 0 = end of stream, -1 = error
int  readMp4AudioAu(Mp4Demuxer *demuxer, AudioAu *au);        // audio-only: 1 = got AU, 0 = end of stream
void closeMp4Demuxer(Mp4Demuxer *demuxer);

// jumps to a keyframe and clears the audio queue (only call with the audio consumer parked). landAfter=0
// lands on the keyframe at/before targetNs; landAfter=1 lands on the next keyframe at/after it (skip past a
// segment). Returns the time actually landed on.
uint64_t seekMp4Demuxer(Mp4Demuxer *demuxer, uint64_t targetNs, int landAfter);
