#pragma once

// audio - multi-stream audio mixer with wav and ogg vorbis support

#include <stdint.h>
#include <cell/audio.h>

#define SFX_DEFAULT_VOLUME  1.0f
#define SFX_DEFAULT_SPEED   1.0f
#define SFX_LOOP    0

#define SFX_MAX_STREAMS     8
#define SFX_SAMPLE_RATE     48000
#define SFX_BLOCK_SAMPLES   CELL_AUDIO_BLOCK_SAMPLES

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
} Audio;

int   initSfx(void);
void  termSfx(void);
Audio loadSfx(const char *path, SfxMode mode);
void  freeSfx(Audio *a);
void  playSfx(Audio *a, float volume, float speed, int loop);
void  stopSfx(Audio *a);
void  pauseSfx(Audio *a);
void  resumeSfx(Audio *a);
void  setSfxMasterVolume(float vol);
void  raiseSfxMasterVolume(float amount);
void  lowerSfxMasterVolume(float amount);


