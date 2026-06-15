#pragma once

// audio - multi-stream audio mixer: wav, ogg/vorbis, mp3 and flac (see audio.c)

#include <stdint.h>
#include <cell/audio.h>

#define SFX_DEFAULT_VOLUME  1.0f
#define SFX_DEFAULT_SPEED   1.0f
#define SFX_LOOP    0

// rolling waveform envelope the mixer maintains per stream (newest sample at the end), so a
// UI can show a live visualisation without owning the PCM -- works for streamed audio too.
#define SFX_VIZ_BINS  96

// max source frames a stream decodes per mixer block; also the size of the resample carry buffer.
#define SFX_STREAM_FRAMES  512

struct stb_vorbis;

typedef enum {
    SFX_MEMORY,
    SFX_STREAM
} SfxMode;

typedef enum {
    SFX_STATE_STOPPED,
    SFX_STATE_PLAYING,
    SFX_STATE_PAUSED
} SfxState;

typedef struct {
    float volume;
    float speed;
    int loop;

    // volume fade (ramped on the mixer thread): volume moves toward fadeTarget by fadeStep
    // each audio block; 0 step = not fading. Fading to 0 auto-stops the stream.
    float fadeTarget;
    float fadeStep;

    SfxState state;
    SfxMode mode;
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
    float vizPeaks[SFX_VIZ_BINS]; // rolling recent-amplitude envelope, filled by the mixer

    // mp3 streaming (SFX_STREAM): decoded on the fly from the compressed bytes via dr_mp3, the same
    // way ogg is handled. mp3 is an opaque drmp3* (kept out of this header); mp3Data is the compressed
    // file buffer dr_mp3 reads from and must outlive it; mp3SeekPoints is the client-owned seek table
    // bound into the decoder so seeking is accurate (MP3 has no native sample-accurate seek).
    void    *mp3;
    uint8_t *mp3Data;
    void    *mp3SeekPoints;

    // flac streaming (SFX_STREAM): same idea as mp3, via dr_flac. flac is an opaque drflac* (owned by
    // dr_flac, closed with drflac_close); flacData is the compressed buffer it reads from.
    void    *flac;
    uint8_t *flacData;

    // wav disk streaming (SFX_STREAM): dr_wav reading from an open file via callbacks, so an hour-long
    // wav plays without loading into memory. wav is an opaque drwav*; wavFd is the fd it reads from.
    void    *wav;
    int      wavFd;

    // streaming resample carry (ogg/mp3/flac): decoded-but-unconsumed source frames plus the
    // fractional read position, kept across mixer blocks so resampling stays phase-continuous
    // (no per-block discontinuity / crackle).
    float    srcCarry[SFX_STREAM_FRAMES * 2];   // interleaved, up to 2 channels
    int      srcCarryFrames;
    double   srcCarryPos;

    char     title[64];   // track title from tags (ID3 / Vorbis comment), empty if none
} Audio;

int   initSfx(void);
void  termSfx(void);
Audio loadSfx(const char *path, SfxMode mode);
// Same, from an in-memory WAV/OGG blob (e.g. an asset already read from an archive).
// Copies what it needs, so the caller may free `data` immediately.
Audio loadSfxMem(const void *data, uint32_t size, SfxMode mode);
void  freeSfx(Audio *a);
void  playSfx(Audio *a, float volume, float speed, int loop);
// one-shot at default volume/speed, no looping. safe on a NULL handle so
// callers do not need to null-check optional ui sounds.
static inline void playSfxOnce(Audio *a)
{
    if (a) playSfx(a, SFX_DEFAULT_VOLUME, SFX_DEFAULT_SPEED, 0);
}
void  stopSfx(Audio *a);
// Ramps a stream's volume to `target` over `seconds` (0 = jump instantly). Fading to 0
// stops the stream when it reaches silence. Runs on the mixer thread; cheap to call.
void  fadeSfx(Audio *a, float target, float seconds);
// Jumps playback to `seconds` from the start, clamped to the clip length. Works for memory clips and
// every stream type (stream seeks are handed to the mixer thread to avoid racing the decoder).
void  seekSfx(Audio *a, float seconds);
// Copies up to `maxBins` of the rolling waveform envelope (0..1) into `out`, newest last. Returns
// the number written. Valid for any playing stream regardless of mode.
int   getSfxWaveform(const Audio *a, float *out, int maxBins);
// Sets one stream's own volume (0..1), independent of the master volume. Takes effect on the
// next mixer block. Unlike fadeSfx, reaching 0 leaves the stream playing (silently).
void  setSfxVolume(Audio *a, float volume);
// Elapsed and total play time in seconds (0 when unknown). Valid for memory clips and all streams.
float getSfxPositionSeconds(const Audio *a);
float getSfxDurationSeconds(const Audio *a);
// True when the file extension is one the mixer can decode (wav, ogg, mp3 or flac). Usable as a
// dir-playlist FileFilter for folder navigation.
int   isPlayableAudioFile(const char *name);
void  pauseSfx(Audio *a);
void  resumeSfx(Audio *a);
void  setSfxMasterVolume(float vol);
void  raiseSfxMasterVolume(float amount);
void  lowerSfxMasterVolume(float amount);
