#pragma once

// audio - multi-stream audio mixer: wav, ogg/vorbis, mp3 and flac (see audio.c)

#include <stdint.h>
#include <cell/audio.h>

#define AUDIO_DEFAULT_VOLUME  1.0f
#define AUDIO_DEFAULT_SPEED   1.0f
#define AUDIO_NO_LOOP    0

// rolling waveform envelope the mixer maintains per stream (newest sample at the end), so a
// UI can show a live visualisation without owning the PCM -- works for streamed audio too.
#define AUDIO_VIZ_BINS  96

// max source frames a stream decodes per mixer block; also the size of the resample carry buffer.
#define AUDIO_STREAM_FRAMES  512

struct stb_vorbis;

typedef enum {
   AUDIO_MEMORY,
   AUDIO_STREAM
} AudioMode;

typedef enum {
   AUDIO_STATE_STOPPED,
   AUDIO_STATE_PLAYING,
   AUDIO_STATE_PAUSED
} AudioState;

typedef struct {
   float volume;
   float speed;
   int loop;

   // volume fade (ramped on the mixer thread): volume moves toward fadeTarget by fadeStep
   // each audio block; 0 step = not fading. Fading to 0 auto-stops the stream.
   float fadeTarget;
   float fadeStep;

   AudioState state;
   AudioMode mode;
   int sampleRate;
   int channels;

   float *pcmData;
   uint32_t pcmSamples;
   double playPos;

   struct stb_vorbis *vorbis;
   uint8_t *vorbisFileData;
   uint32_t vorbisFileSize;

   int isOgg;

   int   seekRequest;            // pending stream seek (sample index), applied by the mixer; -1 = none
   float vizPeaks[AUDIO_VIZ_BINS]; // rolling recent-amplitude envelope, filled by the mixer

   // mp3 streaming (AUDIO_STREAM): decoded on the fly from the compressed bytes via dr_mp3, the same
   // way ogg is handled. mp3 is an opaque drmp3* (kept out of this header); mp3Data is the compressed
   // file buffer dr_mp3 reads from and must outlive it; mp3SeekPoints is the client-owned seek table
   // bound into the decoder so seeking is accurate (MP3 has no native sample-accurate seek).
   void    *mp3;
   uint8_t *mp3Data;
   void    *mp3SeekPoints;

   // flac streaming (AUDIO_STREAM): same idea as mp3, via dr_flac. flac is an opaque drflac* (owned by
   // dr_flac, closed with drflac_close); flacData is the compressed buffer it reads from.
   void    *flac;
   uint8_t *flacData;

   // wav disk streaming (AUDIO_STREAM): dr_wav reading from an open file via callbacks, so an hour-long
   // wav plays without loading into memory. wav is an opaque drwav*; wavFile is the open VfsFile* it
   // reads from (heap-allocated so the pointer survives Audio being returned by value).
   void    *wav;
   void    *wavFile;

   // streaming resample carry (ogg/mp3/flac): decoded-but-unconsumed source frames plus the
   // fractional read position, kept across mixer blocks so resampling stays phase-continuous
   // (no per-block discontinuity / crackle).
   float    srcCarry[AUDIO_STREAM_FRAMES * 2];   // interleaved, up to 2 channels
   int      srcCarryFrames;
   double   srcCarryPos;

   char     title[64];   // track title from tags (ID3 / Vorbis comment), empty if none
} Audio;

int   initAudio(void);
void  termAudio(void);
Audio loadAudio(const char *path, AudioMode mode);
// Same, from an in-memory WAV/OGG blob (e.g. an asset already read from an archive).
// Copies what it needs, so the caller may free `data` immediately.
Audio loadAudioMem(const void *data, uint32_t size, AudioMode mode);
void  freeAudio(Audio *a);
void  playAudio(Audio *a, float volume, float speed, int loop);
// one-shot at default volume/speed, no looping. safe on a NULL handle so
// callers do not need to null-check optional ui sounds.
static inline void playAudioOnce(Audio *a)
{
   if (a) playAudio(a, AUDIO_DEFAULT_VOLUME, AUDIO_DEFAULT_SPEED, 0);
}
void  stopAudio(Audio *a);
// Ramps a stream's volume to `target` over `seconds` (0 = jump instantly). Fading to 0
// stops the stream when it reaches silence. Runs on the mixer thread; cheap to call.
void  fadeAudio(Audio *a, float target, float seconds);
// Jumps playback to `seconds` from the start, clamped to the clip length. Works for memory clips and
// every stream type (stream seeks are handed to the mixer thread to avoid racing the decoder).
void  seekAudio(Audio *a, float seconds);
// Copies up to `maxBins` of the rolling waveform envelope (0..1) into `out`, newest last. Returns
// the number written. Valid for any playing stream regardless of mode.
int   getAudioWaveform(const Audio *a, float *out, int maxBins);
// Sets one stream's own volume (0..1), independent of the master volume. Takes effect on the
// next mixer block. Unlike fadeAudio, reaching 0 leaves the stream playing (silently).
void  setAudioVolume(Audio *a, float volume);
// Elapsed and total play time in seconds (0 when unknown). Valid for memory clips and all streams.
float getAudioPositionSeconds(const Audio *a);
float getAudioDurationSeconds(const Audio *a);
// True when the file extension is one the mixer can decode (wav, ogg, mp3 or flac). Usable as a
// dir-playlist FileFilter for folder navigation.
int   isPlayableAudioFile(const char *name);
void  pauseAudio(Audio *a);
void  resumeAudio(Audio *a);
void  setAudioMasterVolume(float vol);
void  raiseAudioMasterVolume(float amount);
void  lowerAudioMasterVolume(float amount);

// External PCM feed (video playback): a ring the video player pushes decoded audio into; the mixer
// drains it into each block alongside the normal streams, resampling to its own rate. Interleaved
// stereo float32 at `sampleRate`. One feed at a time. The consumed-frames counter is the A/V clock:
// it advances only as samples actually reach the speakers' buffer.
// primeFrames is the backlog the mixer builds before it starts playing: it absorbs late-arriving
// audio, and it is also pure added delay. file playback wants a big one (~0.35s, the default when
// 0 is passed); live streaming wants a small one, because there the delay is the whole point.
int      openAudioPcmFeed(int sampleRate, int primeFrames);         // 0 ok, -1 if busy/OOM
void     closeAudioPcmFeed(void);
int      pushAudioPcm(const float *stereoFrames, int frameCount);   // frames accepted (< frameCount when the ring is full)
int      getAudioPcmFeedSpace(void);                                // frames currently pushable
uint64_t getAudioPcmFeedPlayedFrames(void);                         // source frames consumed since open/flush
void     setAudioPcmFeedVolume(float volume);                       // 0..1, default 1
void     pauseAudioPcmFeed(int paused);                             // paused: feed silent, clock frozen
void     flushAudioPcmFeed(void);                                   // drops buffered frames and zeroes the consumed count (seek)
