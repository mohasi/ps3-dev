// audio - multi-stream audio mixer with wav and ogg vorbis support
#include "audio.h"
#include "file.h"
#include "thread.h"
#include <cell/audio.h>
#include <cell/sysmodule.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// reads entire file into a malloc'd buffer. caller frees. NULL on failure.
static uint8_t *readFileAlloc(const char *path, uint32_t *outSize)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return NULL;
    CellFsStat st;
    if (cellFsFstat(fd, &st) != CELL_FS_SUCCEEDED) { cellFsClose(fd); return NULL; }
    uint32_t size = (uint32_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) { cellFsClose(fd); return NULL; }
    uint64_t totalRead = 0;
    while (totalRead < size) {
        uint64_t r;
        if (cellFsRead(fd, buf + totalRead, size - totalRead, &r) != CELL_FS_SUCCEEDED || r == 0) break;
        totalRead += r;
    }
    cellFsClose(fd);
    if (totalRead != size) { free(buf); return NULL; }
    *outSize = size;
    return buf;
}

#define STB_VORBIS_BIG_ENDIAN
#define STB_VORBIS_NO_STDIO
#include <alloca.h>
#include "vorbis.c"

// ============================================================================
// wav helpers
// ============================================================================

typedef struct {
    char riff[4];
    uint32_t fileSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
} WavHeader;

static inline uint16_t wavSwap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

static inline uint32_t wavSwap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

static inline int16_t wavSwapS16(int16_t v) {
    return (int16_t)wavSwap16((uint16_t)v);
}

// ============================================================================
// mixer state
// ============================================================================

static Audio *streams[SFX_MAX_STREAMS];
static int streamCount = 0;
static float masterVolume = 1.0f;
static sys_ppu_thread_t audioThread;
static volatile int audioRunning = 0;
static CellAudioPortConfig portConfig;
static uint32_t portNum;

// ============================================================================
// wav decode
// ============================================================================

static float *decodeWav(const uint8_t *data, uint32_t dataSize,
                        int *outCh, int *outRate, uint32_t *outFrames) {
    if (dataSize < sizeof(WavHeader)) return NULL;

    WavHeader hdr;
    memcpy(&hdr, data, sizeof(WavHeader));
    hdr.fileSize     = wavSwap32(hdr.fileSize);
    hdr.fmtSize      = wavSwap32(hdr.fmtSize);
    hdr.audioFormat   = wavSwap16(hdr.audioFormat);
    hdr.channels      = wavSwap16(hdr.channels);
    hdr.sampleRate    = wavSwap32(hdr.sampleRate);
    hdr.byteRate      = wavSwap32(hdr.byteRate);
    hdr.blockAlign    = wavSwap16(hdr.blockAlign);
    hdr.bitsPerSample = wavSwap16(hdr.bitsPerSample);

    if (hdr.audioFormat != 1) return NULL;

    uint32_t off = 12;
    while (off + 8 <= dataSize) {
        char id[4];
        uint32_t csz;
        memcpy(id, data + off, 4);
        memcpy(&csz, data + off + 4, 4);
        csz = wavSwap32(csz);

        if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            uint32_t nf = csz / (hdr.channels * (hdr.bitsPerSample / 8));
            float *pcm = (float *)malloc(nf * hdr.channels * sizeof(float));
            if (!pcm) return NULL;

            if (hdr.bitsPerSample == 16) {
                const int16_t *s = (const int16_t *)(data + off + 8);
                for (uint32_t i = 0; i < nf * hdr.channels; i++)
                    pcm[i] = wavSwapS16(s[i]) / 32768.0f;
            } else if (hdr.bitsPerSample == 8) {
                const uint8_t *s = data + off + 8;
                for (uint32_t i = 0; i < nf * hdr.channels; i++)
                    pcm[i] = (s[i] - 128) / 128.0f;
            } else {
                free(pcm);
                return NULL;
            }

            *outCh = hdr.channels;
            *outRate = hdr.sampleRate;
            *outFrames = nf;
            return pcm;
        }

        off += 8 + csz;
        if (csz & 1) off++;
    }
    return NULL;
}

// ============================================================================
// ogg decode (full memory)
// ============================================================================

static float *decodeOggMemory(const uint8_t *data, uint32_t dataSize,
                              int *outCh, int *outRate, uint32_t *outFrames) {
    int ch, sr;
    short *raw = NULL;
    int n = stb_vorbis_decode_memory(data, (int)dataSize, &ch, &sr, &raw);
    if (n <= 0 || !raw) return NULL;

    float *pcm = (float *)malloc(n * ch * sizeof(float));
    if (!pcm) { free(raw); return NULL; }

    for (int i = 0; i < n * ch; i++)
        pcm[i] = raw[i] / 32768.0f;
    free(raw);

    *outCh = ch;
    *outRate = sr;
    *outFrames = (uint32_t)n;
    return pcm;
}

// ============================================================================
// linear resample
// ============================================================================

static float *resamplePcm(const float *src, int ch, int srcRate,
                          int dstRate, uint32_t srcN, uint32_t *outN) {
    if (srcRate == dstRate) {
        *outN = srcN;
        float *c = (float *)malloc(srcN * ch * sizeof(float));
        if (c) memcpy(c, src, srcN * ch * sizeof(float));
        return c;
    }

    double ratio = (double)dstRate / (double)srcRate;
    uint32_t dn = (uint32_t)(srcN * ratio);
    float *dst = (float *)malloc(dn * ch * sizeof(float));
    if (!dst) return NULL;

    for (uint32_t i = 0; i < dn; i++) {
        double sp = i / ratio;
        uint32_t i0 = (uint32_t)sp;
        float f = (float)(sp - i0);
        if (i0 >= srcN) i0 = srcN - 1;
        uint32_t i1 = (i0 + 1 < srcN) ? i0 + 1 : i0;
        for (int c = 0; c < ch; c++)
            dst[i * ch + c] = src[i0 * ch + c] + f * (src[i1 * ch + c] - src[i0 * ch + c]);
    }

    *outN = dn;
    return dst;
}

// ============================================================================
// mixer thread
// ============================================================================

#define STREAM_DECODE_FRAMES 512

static inline void mixSamples(const float *src, uint32_t srcLen, int srcCh,
                              float *mix, double *pos, double step, float vol) {
    for (int i = 0; i < SFX_BLOCK_SAMPLES; i++) {
        uint32_t p = (uint32_t)*pos;
        if (p + 1 >= srcLen) break;
        float fr = (float)(*pos - p);

        if (srcCh >= 2) {
            float l = src[p * 2]     + fr * (src[(p + 1) * 2]     - src[p * 2]);
            float r = src[p * 2 + 1] + fr * (src[(p + 1) * 2 + 1] - src[p * 2 + 1]);
            mix[i * 2]     += l * vol;
            mix[i * 2 + 1] += r * vol;
        } else {
            float m = src[p] + fr * (src[p + 1] - src[p]);
            mix[i * 2]     += m * vol;
            mix[i * 2 + 1] += m * vol;
        }

        *pos += step;
    }
}

static void audioMixerThread(uint64_t arg) {
    (void)arg;
    float mix[SFX_BLOCK_SAMPLES * 2];
    uint64_t lastBlock = 0;

    while (audioRunning) {
        uint64_t curBlock = *(volatile uint64_t *)portConfig.readIndexAddr;
        if (curBlock == lastBlock) {
            sys_timer_usleep(500);
            continue;
        }
        lastBlock = curBlock;

        memset(mix, 0, sizeof(mix));

        for (int s = 0; s < streamCount; s++) {
            Audio *a = streams[s];
            if (!a || a->state != SFX_STATE_PLAYING) continue;

            float vol = a->volume * masterVolume;
            double step = (double)a->sampleRate / (double)SFX_SAMPLE_RATE * a->speed;

            if (a->mode == SFX_MEMORY && a->pcmData) {
                double pos = a->playPos;
                mixSamples(a->pcmData, a->pcmSamples, a->channels, mix, &pos, step, vol);

                if ((uint32_t)pos + 1 >= a->pcmSamples) {
                    if (a->loop) { a->playPos = 0.0; }
                    else { a->state = SFX_STATE_STOPPED; }
                } else {
                    a->playPos = pos;
                }
            } else if (a->mode == SFX_STREAM && a->vorbis) {
                int srcNeeded = (int)(SFX_BLOCK_SAMPLES * step) + 2;
                if (srcNeeded > STREAM_DECODE_FRAMES) srcNeeded = STREAM_DECODE_FRAMES;

                float db[STREAM_DECODE_FRAMES * 2];
                int decoded = 0;

                while (decoded < srcNeeded) {
                    int got = stb_vorbis_get_samples_float_interleaved(
                        a->vorbis, 2, db + decoded * 2, (srcNeeded - decoded) * 2);
                    if (got == 0) {
                        if (a->loop) { stb_vorbis_seek_start(a->vorbis); continue; }
                        else { a->state = SFX_STATE_STOPPED; break; }
                    }
                    decoded += got;
                }

                double pos = 0.0;
                mixSamples(db, (uint32_t)decoded, 2, mix, &pos, step, vol);
            }
        }

        for (int i = 0; i < SFX_BLOCK_SAMPLES * 2; i++) {
            if (mix[i] > 1.0f) mix[i] = 1.0f;
            else if (mix[i] < -1.0f) mix[i] = -1.0f;
        }

        uint32_t writeBlock = (curBlock + 1) % 8;
        float *dst = (float *)(uintptr_t)(portConfig.portAddr +
                     SFX_BLOCK_SAMPLES * 2 * sizeof(float) * writeBlock);
        memcpy(dst, mix, SFX_BLOCK_SAMPLES * 2 * sizeof(float));
    }

    sys_ppu_thread_exit(0);
}

// ============================================================================
// init / term
// ============================================================================

int initSfx(void) {
    cellSysmoduleLoadModule(CELL_SYSMODULE_AUDIO);

    CellAudioPortParam p;
    memset(&p, 0, sizeof(p));
    p.nChannel = 2;
    p.nBlock = 8;
    p.attr = CELL_AUDIO_PORTATTR_BGM;
    p.level = 1.0f;

    if (cellAudioInit() != CELL_OK) return -1;
    if (cellAudioPortOpen(&p, &portNum) != CELL_OK) return -1;
    if (cellAudioGetPortConfig(portNum, &portConfig) != CELL_OK) return -1;
    if (cellAudioPortStart(portNum) != CELL_OK) return -1;

    audioRunning = 1;
    streamCount = 0;
    memset(streams, 0, sizeof(streams));

    sys_ppu_thread_create(&audioThread, audioMixerThread, 0,
                          THREAD_PRIORITY_HIGH, THREAD_STACK_SIZE_16KB,
                          SYS_PPU_THREAD_CREATE_JOINABLE, "audio-mixer");
    return 0;
}

void termSfx(void) {
    audioRunning = 0;
    uint64_t ec;
    sys_ppu_thread_join(audioThread, &ec);

    cellAudioPortStop(portNum);
    cellAudioPortClose(portNum);
    cellAudioQuit();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_AUDIO);
}

// ============================================================================
// load / free
// ============================================================================

Audio loadSfx(const char *path, SfxMode mode) {
    Audio a;
    memset(&a, 0, sizeof(a));
    a.volume = SFX_DEFAULT_VOLUME;
    a.speed  = SFX_DEFAULT_SPEED;
    a.loop   = SFX_LOOP;
    a.state  = SFX_STATE_STOPPED;
    a.mode   = mode;

    uint32_t fsz = 0;
    uint8_t *fd = readFileAlloc(path, &fsz);
    if (!fd || fsz < 4) { if (fd) free(fd); return a; }

    a.isOgg = (fd[0] == 'O' && fd[1] == 'g' && fd[2] == 'g' && fd[3] == 'S');

    if (mode == SFX_MEMORY) {
        int ch, sr;
        uint32_t nf;
        float *pcm = a.isOgg ? decodeOggMemory(fd, fsz, &ch, &sr, &nf)
                              : decodeWav(fd, fsz, &ch, &sr, &nf);
        free(fd);
        if (!pcm) return a;

        if (sr != SFX_SAMPLE_RATE) {
            uint32_t rn;
            float *rp = resamplePcm(pcm, ch, sr, SFX_SAMPLE_RATE, nf, &rn);
            free(pcm);
            if (!rp) return a;
            pcm = rp; nf = rn; sr = SFX_SAMPLE_RATE;
        }

        a.pcmData = pcm;
        a.pcmSamples = nf;
        a.sampleRate = sr;
        a.channels = ch;
    } else {
        if (!a.isOgg) {
            int ch, sr;
            uint32_t nf;
            float *pcm = decodeWav(fd, fsz, &ch, &sr, &nf);
            free(fd);
            if (!pcm) return a;

            if (sr != SFX_SAMPLE_RATE) {
                uint32_t rn;
                float *rp = resamplePcm(pcm, ch, sr, SFX_SAMPLE_RATE, nf, &rn);
                free(pcm);
                if (!rp) return a;
                pcm = rp; nf = rn;
            }

            a.pcmData = pcm;
            a.pcmSamples = nf;
            a.sampleRate = SFX_SAMPLE_RATE;
            a.channels = ch;
            a.mode = SFX_MEMORY;
        } else {
            int err;
            stb_vorbis *v = stb_vorbis_open_memory(fd, (int)fsz, &err, NULL);
            if (!v) { free(fd); return a; }

            stb_vorbis_info info = stb_vorbis_get_info(v);
            a.vorbis = v;
            a.vorbisFileData = fd;
            a.vorbisFileSize = fsz;
            a.sampleRate = info.sample_rate;
            a.channels = info.channels;
        }
    }

    return a;
}

void freeSfx(Audio *a) {
    stopSfx(a);
    if (a->pcmData)        { free(a->pcmData);        a->pcmData = NULL; }
    if (a->vorbis)         { stb_vorbis_close(a->vorbis); a->vorbis = NULL; }
    if (a->vorbisFileData) { free(a->vorbisFileData);  a->vorbisFileData = NULL; }
}

// ============================================================================
// playback control
// ============================================================================

void playSfx(Audio *a, float volume, float speed, int loop) {
    a->volume = volume;
    a->speed  = speed;
    a->loop   = loop;
    a->playPos = 0.0;
    a->state  = SFX_STATE_PLAYING;

    if (a->mode == SFX_STREAM && a->vorbis)
        stb_vorbis_seek_start(a->vorbis);

    for (int i = 0; i < streamCount; i++)
        if (streams[i] == a) return;
    if (streamCount < SFX_MAX_STREAMS)
        streams[streamCount++] = a;
}

void stopSfx(Audio *a) {
    a->state = SFX_STATE_STOPPED;
    a->playPos = 0.0;

    for (int i = 0; i < streamCount; i++) {
        if (streams[i] == a) {
            streams[i] = streams[streamCount - 1];
            streams[streamCount - 1] = NULL;
            streamCount--;
            break;
        }
    }
}

void pauseSfx(Audio *a) {
    if (a->state == SFX_STATE_PLAYING)
        a->state = SFX_STATE_PAUSED;
}

void resumeSfx(Audio *a) {
    if (a->state == SFX_STATE_PAUSED)
        a->state = SFX_STATE_PLAYING;
}

void setSfxMasterVolume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    masterVolume = vol;
}

void raiseSfxMasterVolume(float amount) {
    setSfxMasterVolume(masterVolume + amount);
}

void lowerSfxMasterVolume(float amount) {
    setSfxMasterVolume(masterVolume - amount);
}
