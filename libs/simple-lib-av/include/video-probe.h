#pragma once

// video-probe - header-only container inspection (no full demux) that decides whether the PS3 can
// actually play a given video file. Parses just the track metadata of an MKV (EBML) or MP4 (ISOBMFF)
// container and reports the video/audio codecs found plus a playability verdict.
//
// PS3 decode limits: 8-bit H.264 / MPEG-2 / DivX video (no HEVC, no 10-bit); AAC / AC3 / MP3 audio
// (no E-AC3 / Opus / FLAC / DTS). The v1 player itself decodes H.264 + AAC; the probe reports what
// the hardware could decode so the overlay can show a precise reason when it can't.

#include "video-source.h"

typedef enum {
   VIDEO_CODEC_NONE,
   VIDEO_CODEC_H264,
   VIDEO_CODEC_HEVC,
   VIDEO_CODEC_MPEG2,
   VIDEO_CODEC_MPEG4,    // MPEG-4 ASP (DivX / Xvid)
   VIDEO_CODEC_OTHER
} VideoCodec;

typedef enum {
   AUDIO_CODEC_NONE,
   AUDIO_CODEC_AAC,
   AUDIO_CODEC_AC3,
   AUDIO_CODEC_MP3,
   AUDIO_CODEC_OTHER
} VideoAudioCodec;

typedef enum {
   VIDEO_PLAYABLE,      // the PS3 can decode the video track (v1: 8-bit H.264)
   VIDEO_UNSUPPORTED,   // container parsed, but the video codec/profile can't be decoded
   VIDEO_UNREADABLE     // couldn't recognise or parse the container
} VideoVerdict;

typedef struct {
   VideoVerdict verdict;
   char reason[256];    // human message when not playable (empty when playable)

   VideoCodec      videoCodec;
   int             width, height;
   int             bitDepth;      // 8 or 10; 0 if unknown
   int             maxRefFrames;  // H.264 SPS max_num_ref_frames; 0 if unknown
   int             refFramesExceedDpb;   // 1 when the stream needs more ref frames than the PS3 decoder holds
   VideoAudioCodec audioCodec;

   char videoCodecName[40];     // best-effort raw codec id, for the message
   char audioCodecName[40];
} VideoPlayability;

// classifies by extension whether the player should offer to open this path (mkv/mp4/...).
int isVideoFile(const char *name);

// inspects the container at `path` and fills `out`. returns out->verdict for convenience.
VideoVerdict probeVideo(const char *path, VideoPlayability *out);
