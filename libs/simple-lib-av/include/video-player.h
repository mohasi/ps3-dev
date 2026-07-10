#pragma once

// video-player - the playback engine. Owns the demuxer (MKV or MP4), the H.264 decoder and a
// background thread that decodes ahead into a small ring of frames. The UI presents frames by
// calling getVideoFrame each render frame; the engine hands back the frame whose presentation time
// is due (skipping ahead if the UI fell behind, holding if it's early), so playback runs at the
// file's real frame rate. The AAC audio track drives the clock while it plays; wall-time otherwise.

#include <stdint.h>

typedef struct VideoPlayer VideoPlayer;

// frame-buffer allocator hooks. The UI supplies these so frames land in RSX-visible memory and are
// drawn zero-copy (gfx's allocGfxVideoBuffer/freeGfxVideoBuffer); pass NULL for plain heap buffers.
typedef void *(*VideoFrameAllocFn)(size_t size);
typedef void  (*VideoFrameFreeFn)(void *buffer);

// opens `path`, brings up the decoder, and starts decoding. Returns NULL if the file can't be
// demuxed / decoded (caller should have probed first for a user-facing reason).
VideoPlayer *createVideoPlayer(const char *path, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame);

// like createVideoPlayer, but the audio comes from a SEPARATE source (adaptive/DASH: a video-only
// url plus an audio-only url). audioPath NULL means the audio is muxed into videoPath. A/V stay in
// sync because both share one presentation timeline and the audio drives the clock.
VideoPlayer *createVideoPlayerSplit(const char *videoPath, const char *audioPath, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame);
void         destroyVideoPlayer(VideoPlayer *player);

// returns the YUV 4:2:0 planar frame (Y then U then V, width*height*3/2 bytes) that should be shown
// now (paced against the clock), or NULL if none is ready yet. The pointer stays valid until the
// SECOND-next getVideoFrame call (the previous frame is retired one call late so the GPU is never
// reading a buffer the decoder is refilling). Fills *width/*height with the coded plane dimensions.
const uint8_t *getVideoFrame(VideoPlayer *player, int *width, int *height);

void  setVideoPaused(VideoPlayer *player, int paused);
int   isVideoPaused(const VideoPlayer *player);
// asynchronous seek: decode restarts from the nearest cued keyframe at or before `seconds` (both
// pipelines flush; the current frame stays on screen until the new position's frames arrive).
// Seeking after the end restarts playback.
void  seekVideoPlayer(VideoPlayer *player, float seconds);
// like seekVideoPlayer but lands on the next keyframe at or AFTER `seconds`, so a skip clears the whole
// span (e.g. a sponsor segment) instead of resuming on the keyframe before it and replaying the tail.
void  seekVideoPlayerPast(VideoPlayer *player, float seconds);
int   isVideoEnded(const VideoPlayer *player);       // decode reached end of stream and the ring drained
float getVideoPositionSeconds(const VideoPlayer *player);
float getVideoDurationSeconds(const VideoPlayer *player);
void  getAudioTrackInfo(const VideoPlayer *player, int *rate, int *channels);   // 0/0 if no audio track
