// audio - multi-stream audio mixer: wav and ogg/vorbis, plus mp3 and flac via vendored decoders
#include "audio.h"
#include "file.h"               // getExtension
#include "string-utilities.h"   // strCmpICase
#include "thread.h"
#include <cell/audio.h>
#include <cell/sysmodule.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <string.h>
#include <stdlib.h>

#define SFX_MAX_STREAMS    8
#define SFX_SAMPLE_RATE    48000
#define SFX_BLOCK_SAMPLES  CELL_AUDIO_BLOCK_SAMPLES
#define SFX_PORT_BLOCKS    8     // audio-port ring-buffer block count (cellAudioPortOpen nBlock)

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

// ---------------------------------------------------------------------------
// Audio decoding uses these vendored, single-header, public-domain libraries:
//   - stb_vorbis  (vorbis.h) - Ogg Vorbis  - Sean Barrett (nothings)
//   - dr_mp3      (mp3.h)    - MP3         - David Reid (mackron / dr_libs); core from minimp3 by Lion (lieff)
//   - dr_flac     (flac.h)   - FLAC        - David Reid (mackron / dr_libs)
//   - dr_wav      (wav.h)    - WAV         - David Reid (mackron / dr_libs)
// ---------------------------------------------------------------------------
// vendored single-header decoders (vorbis is the stb amalgamation, named .h for consistency;
// dr_mp3/dr_flac gate their implementation behind the *_IMPLEMENTATION defines).
#define STB_VORBIS_BIG_ENDIAN
#define STB_VORBIS_NO_STDIO
#include <alloca.h>
#include "vorbis.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "flac.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "wav.h"

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

// The stream the mixer thread is currently reading from (NULL between streams). stopSfx waits on
// this before returning so a following freeSfx can't release a decoder mid-block. See stopSfx.
static Audio * volatile mixingStream = NULL;

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

// peak absolute amplitude of the left/mono channel over `frames` interleaved samples.
static inline float blockPeak(const float *interleaved, int frames, int ch) {
    float peak = 0.0f;
    for (int i = 0; i < frames; i++) {
        float s = interleaved[i * ch];
        if (s < 0.0f) s = -s;
        if (s > peak) peak = s;
    }
    return peak;
}

// scrolls one new peak into the rolling waveform envelope (oldest drops off the front).
static inline void appendSfxViz(Audio *a, float peak) {
    memmove(a->vizPeaks, a->vizPeaks + 1, (SFX_VIZ_BINS - 1) * sizeof(float));
    a->vizPeaks[SFX_VIZ_BINS - 1] = peak;
}

// ----- pull-decoder streaming (ogg/mp3/flac/wav) -----
// number of interleaved channels a stream decodes into (ogg is always decoded as stereo).
static inline int streamChannels(const Audio *a) { return a->vorbis ? 2 : a->channels; }

// pulls up to `count` source frames from the stream's decoder into dst (interleaved). 0 = end.
static int decodeStream(Audio *a, float *dst, int count) {
    if (a->vorbis) return stb_vorbis_get_samples_float_interleaved(a->vorbis, 2, dst, count * 2);
    if (a->mp3)    return (int)drmp3_read_pcm_frames_f32((drmp3 *)a->mp3, (drmp3_uint64)count, dst);
    if (a->flac)   return (int)drflac_read_pcm_frames_f32((drflac *)a->flac, (drflac_uint64)count, dst);
    if (a->wav)    return (int)drwav_read_pcm_frames_f32((drwav *)a->wav, (drwav_uint64)count, dst);
    return 0;
}

// seeks the stream's decoder to a source frame (used for user seeks and looping).
static void seekStream(Audio *a, uint32_t frame) {
    if (a->vorbis)    stb_vorbis_seek(a->vorbis, frame);
    else if (a->mp3)  drmp3_seek_to_pcm_frame((drmp3 *)a->mp3, frame);
    else if (a->flac) drflac_seek_to_pcm_frame((drflac *)a->flac, frame);
    else if (a->wav)  drwav_seek_to_pcm_frame((drwav *)a->wav, frame);
}

// one mixer block for a pull-decoder stream. The resample carry (decoded-but-unconsumed frames +
// fractional position) persists across blocks, so resampling stays phase-continuous instead of
// restarting (and dropping a couple of frames) every block.
static void mixStreamBlock(Audio *a, float *mix, double step, float vol) {
    int ch = streamChannels(a);

    if (a->seekRequest >= 0) {
        seekStream(a, (uint32_t)a->seekRequest);
        a->playPos = (double)a->seekRequest;
        a->seekRequest = -1;
        a->srcCarryFrames = 0;
        a->srcCarryPos = 0.0;
    }

    // top up the carry buffer so it covers this block's resample span (fractional pos + 256*step)
    int need = (int)(a->srcCarryPos + SFX_BLOCK_SAMPLES * step) + 2;
    if (need > SFX_STREAM_FRAMES) need = SFX_STREAM_FRAMES;

    int ended = 0, looped = 0;
    while (a->srcCarryFrames < need) {
        int got = decodeStream(a, a->srcCarry + a->srcCarryFrames * ch, need - a->srcCarryFrames);
        if (got == 0) {
            if (a->loop && !looped) { seekStream(a, 0); looped = 1; continue; }   // wrap once
            ended = 1; break;
        }
        a->srcCarryFrames += got;
    }

    if (a->srcCarryFrames > 0) {
        double pos = a->srcCarryPos;
        mixSamples(a->srcCarry, (uint32_t)a->srcCarryFrames, ch, mix, &pos, step, vol);

        int consumed = (int)pos;
        if (consumed > a->srcCarryFrames) consumed = a->srcCarryFrames;
        appendSfxViz(a, blockPeak(a->srcCarry, consumed > 0 ? consumed : a->srcCarryFrames, ch));

        // keep the unconsumed tail + fractional position for the next block
        int remaining = a->srcCarryFrames - consumed;
        if (remaining > 0)
            memmove(a->srcCarry, a->srcCarry + consumed * ch, (size_t)remaining * ch * sizeof(float));
        a->srcCarryFrames = remaining;
        a->srcCarryPos = pos - consumed;
        a->playPos += consumed;
    }

    if (ended && a->srcCarryFrames == 0 && !a->loop) a->state = SFX_STATE_STOPPED;
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
            if (!a) continue;

            // Claim the stream BEFORE re-reading its state. Paired with the fence in stopSfx this
            // is a mutual-flag handshake: stopSfx either sees this claim and waits, or its STOPPED
            // store is visible just below and we bail -- so we never touch a decoder being freed.
            mixingStream = a;
            __sync_synchronize();
            if (a->state != SFX_STATE_PLAYING) { mixingStream = NULL; continue; }

            // advance a volume fade by one block, if one is running
            if (a->fadeStep != 0.0f) {
                a->volume += a->fadeStep;
                if ((a->fadeStep > 0.0f && a->volume >= a->fadeTarget) ||
                    (a->fadeStep < 0.0f && a->volume <= a->fadeTarget)) {
                    a->volume = a->fadeTarget;
                    a->fadeStep = 0.0f;
                    if (a->fadeTarget <= 0.0f) a->state = SFX_STATE_STOPPED;   // faded out
                }
            }

            float vol = a->volume * masterVolume;
            double step = (double)a->sampleRate / (double)SFX_SAMPLE_RATE * a->speed;

            if (a->mode == SFX_MEMORY && a->pcmData) {
                double startPos = a->playPos;
                double pos = a->playPos;
                mixSamples(a->pcmData, a->pcmSamples, a->channels, mix, &pos, step, vol);

                int frames = (int)(pos - startPos);
                if (frames > 0)
                    appendSfxViz(a, blockPeak(a->pcmData + (uint32_t)startPos * a->channels, frames, a->channels));

                if ((uint32_t)pos + 1 >= a->pcmSamples) {
                    if (a->loop) { a->playPos = 0.0; }
                    else { a->state = SFX_STATE_STOPPED; }
                } else {
                    a->playPos = pos;
                }
            } else if (a->mode == SFX_STREAM && (a->vorbis || a->mp3 || a->flac || a->wav)) {
                mixStreamBlock(a, mix, step, vol);
            }

            __sync_synchronize();
            mixingStream = NULL;   // released; safe for freeSfx to tear this stream down now
        }

        for (int i = 0; i < SFX_BLOCK_SAMPLES * 2; i++) {
            if (mix[i] > 1.0f) mix[i] = 1.0f;
            else if (mix[i] < -1.0f) mix[i] = -1.0f;
        }

        uint32_t writeBlock = (curBlock + 1) % SFX_PORT_BLOCKS;
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
    p.nBlock = SFX_PORT_BLOCKS;
    p.attr = CELL_AUDIO_PORTATTR_BGM;
    p.level = 1.0f;

    if (cellAudioInit() != CELL_OK) return -1;
    if (cellAudioPortOpen(&p, &portNum) != CELL_OK) return -1;
    if (cellAudioGetPortConfig(portNum, &portConfig) != CELL_OK) return -1;
    if (cellAudioPortStart(portNum) != CELL_OK) return -1;

    audioRunning = 1;
    streamCount = 0;
    memset(streams, 0, sizeof(streams));

    // 64KB stack: the mixer decodes ogg and converts wav PCM in per-block buffers on this thread.
    sys_ppu_thread_create(&audioThread, audioMixerThread, 0,
                          THREAD_PRIORITY_HIGH, THREAD_STACK_SIZE_64KB,
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

// ----- track title from tags -----

// if `entry` (length `len`, not necessarily NUL-terminated) is a "TITLE=..." vorbis comment, copy
// its value into out. Case-insensitive key; leaves out unchanged otherwise.
static void takeVorbisTitle(const char *entry, int len, char *out, int cap) {
    static const char key[] = "TITLE=";
    int keyLen = 6;
    if (len < keyLen) return;
    for (int i = 0; i < keyLen; i++) if (toLowerChar(entry[i]) != toLowerChar(key[i])) return;
    int n = len - keyLen;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, entry + keyLen, n);
    out[n] = '\0';
}

// ogg: read the TITLE vorbis comment from the open decoder.
static void readOggTitle(Audio *a) {
    stb_vorbis_comment c = stb_vorbis_get_comment(a->vorbis);
    for (int i = 0; i < c.comment_list_length && a->title[0] == '\0'; i++)
        if (c.comment_list[i]) takeVorbisTitle(c.comment_list[i], (int)strlen(c.comment_list[i]), a->title, sizeof a->title);
}

// flac: drflac metadata callback; pulls TITLE out of the VORBIS_COMMENT block during open.
static void onFlacMeta(void *pUserData, drflac_metadata *pMeta) {
    if (pMeta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    Audio *a = (Audio *)pUserData;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, pMeta->data.vorbis_comment.commentCount, pMeta->data.vorbis_comment.pComments);
    drflac_uint32 len;
    const char *comment;
    while ((comment = drflac_next_vorbis_comment(&it, &len)) != NULL)
        if (a->title[0] == '\0') takeVorbisTitle(comment, (int)len, a->title, sizeof a->title);
}

// copies an ID3 text frame body (after its 1-byte encoding) into out. Handles latin1/utf8 directly
// and pulls ASCII out of UTF-16 by taking the non-zero byte of each unit (good enough for display).
static void copyId3Text(uint8_t enc, const uint8_t *s, uint32_t len, char *out, int cap) {
    int o = 0;
    if (enc == 1 || enc == 2) {                       // UTF-16 (with/without BOM)
        uint32_t i = (len >= 2 && (s[0] == 0xFF || s[0] == 0xFE)) ? 2 : 0;   // skip BOM
        for (; i + 1 < len && o < cap - 1; i += 2) {
            char ch = s[i] ? (char)s[i] : (char)s[i + 1];
            if (ch == '\0') break;
            out[o++] = ch;
        }
    } else {                                          // 0 = latin1, 3 = utf8
        for (uint32_t i = 0; i < len && o < cap - 1; i++) { if (s[i] == '\0') break; out[o++] = (char)s[i]; }
    }
    out[o] = '\0';
}

// mp3: pull the title from an ID3v2 TIT2 frame, falling back to the ID3v1 trailer.
static void readMp3Title(Audio *a, const uint8_t *d, uint32_t n) {
    if (n > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        uint8_t major = d[3];
        uint32_t tagSize = ((uint32_t)(d[6] & 0x7F) << 21) | ((uint32_t)(d[7] & 0x7F) << 14) |
                           ((uint32_t)(d[8] & 0x7F) << 7)  |  (uint32_t)(d[9] & 0x7F);
        uint32_t end = 10 + tagSize;
        if (end > n) end = n;
        for (uint32_t off = 10; off + 10 <= end; ) {
            const uint8_t *fr = d + off;
            if (fr[0] == 0) break;   // padding
            uint32_t frameSize = (major >= 4)
                ? (((uint32_t)(fr[4] & 0x7F) << 21) | ((uint32_t)(fr[5] & 0x7F) << 14) | ((uint32_t)(fr[6] & 0x7F) << 7) | (uint32_t)(fr[7] & 0x7F))
                : (((uint32_t)fr[4] << 24) | ((uint32_t)fr[5] << 16) | ((uint32_t)fr[6] << 8) | (uint32_t)fr[7]);
            // the frame body (off+10 .. off+10+frameSize) must stay inside the tag, or the file is
            // malformed -- stop rather than read past the buffer.
            if (frameSize == 0 || (uint64_t)off + 10 + frameSize > end) break;
            if (fr[0] == 'T' && fr[1] == 'I' && fr[2] == 'T' && fr[3] == '2') {
                copyId3Text(fr[10], fr + 11, frameSize - 1, a->title, sizeof a->title);
                return;
            }
            off += 10 + frameSize;
        }
    }
    if (a->title[0] == '\0' && n >= 128) {              // ID3v1 trailer: "TAG" + 30-byte title
        const uint8_t *tag = d + n - 128;
        if (tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
            int len = 30;
            while (len > 0 && (tag[3 + len - 1] == ' ' || tag[3 + len - 1] == '\0')) len--;
            if (len > (int)sizeof a->title - 1) len = (int)sizeof a->title - 1;
            memcpy(a->title, tag + 3, len);
            a->title[len] = '\0';
        }
    }
}

// Decodes a whole wav/ogg blob to interleaved float PCM at the mixer's sample rate and stores it in
// `a` for memory playback. Takes ownership of `fd` (frees it); leaves `a` untouched on failure.
static void loadPcmToMemory(uint8_t *fd, uint32_t fsz, int isOgg, Audio *a) {
    int ch = 0, sr = 0;
    uint32_t nf = 0;
    float *pcm;
    if (isOgg) {
        pcm = decodeOggMemory(fd, fsz, &ch, &sr, &nf);
    } else {
        unsigned int wavCh = 0, wavSr = 0;
        drwav_uint64 wavFrames = 0;
        pcm = drwav_open_memory_and_read_pcm_frames_f32(fd, fsz, &wavCh, &wavSr, &wavFrames, NULL);
        ch = (int)wavCh; sr = (int)wavSr; nf = (uint32_t)wavFrames;
    }
    free(fd);
    if (!pcm) return;

    if (sr != SFX_SAMPLE_RATE) {
        uint32_t rn;
        float *rp = resamplePcm(pcm, ch, sr, SFX_SAMPLE_RATE, nf, &rn);
        free(pcm);
        if (!rp) return;
        pcm = rp; nf = rn; sr = SFX_SAMPLE_RATE;
    }
    a->pcmData    = pcm;
    a->pcmSamples = nf;
    a->sampleRate = sr;
    a->channels   = ch;
    a->mode       = SFX_MEMORY;
}

// Counts PCM frames by decoding the whole stream, then rewinds. Last-resort fallback for the rare mp3
// whose length dr_mp3 still can't determine (no Xing count and the scan came back zero), so the seek
// bar has a usable duration. Note: malformed VBR files that declare a zero Xing frame count are now
// handled in mp3.h (it ignores the bogus count and scans), so they no longer reach this path.
static uint32_t mp3FrameCountByScan(drmp3 *mp3) {
    drmp3_seek_to_pcm_frame(mp3, 0);
    uint64_t total = 0;
    float buf[1152 * 2];
    drmp3_uint64 got;
    while ((got = drmp3_read_pcm_frames_f32(mp3, 1152, buf)) > 0) total += got;
    drmp3_seek_to_pcm_frame(mp3, 0);
    return (uint32_t)total;
}

// Builds an Audio from an owned byte buffer (the file contents). Takes ownership of `fd`: frees it
// after decoding (memory mode) or keeps it for the stream's life (ogg/mp3/flac decode from it).
// Shared by loadSfx (reads a file) and loadSfxMem (already-in-memory blob).
static Audio loadSfxBuffer(uint8_t *fd, uint32_t fsz, SfxMode mode) {
    Audio a;
    memset(&a, 0, sizeof(a));
    a.volume = SFX_DEFAULT_VOLUME;
    a.speed  = SFX_DEFAULT_SPEED;
    a.loop   = SFX_LOOP;
    a.state  = SFX_STATE_STOPPED;
    a.mode   = mode;
    a.seekRequest = -1;

    if (!fd || fsz < 4) { if (fd) free(fd); return a; }

    a.isOgg = (fd[0] == 'O' && fd[1] == 'g' && fd[2] == 'g' && fd[3] == 'S');
    int isMp3 = (fd[0] == 'I' && fd[1] == 'D' && fd[2] == '3') ||      // ID3v2 tag
                (fd[0] == 0xFF && (fd[1] & 0xE0) == 0xE0);             // raw MPEG frame sync
    int isFlac = (fd[0] == 'f' && fd[1] == 'L' && fd[2] == 'a' && fd[3] == 'C');

    if (mode == SFX_MEMORY) {
        loadPcmToMemory(fd, fsz, a.isOgg, &a);
    } else if (isMp3) {
        // mp3 stream: dr_mp3 decodes from the compressed bytes (kept in mp3Data) on demand.
        drmp3 *mp3 = (drmp3 *)malloc(sizeof(drmp3));
        if (!mp3 || !drmp3_init_memory(mp3, fd, fsz, NULL)) { if (mp3) free(mp3); free(fd); return a; }
        a.mp3        = mp3;
        a.mp3Data    = fd;                                            // dr_mp3 references this; keep it alive
        a.sampleRate = (int)mp3->sampleRate;
        a.channels   = (int)mp3->channels;

        // Length: dr_mp3 returns the Xing/Info frame count when present, else scans. Our mp3.h carries
        // a local edit that ignores a malformed zero Xing frame count (see mp3.h), so a real scan gives
        // the true length here -- e.g. VBR files like "8_mile_...lose_yourself.mp3" that declare 0.
        uint32_t rawCount = (uint32_t)drmp3_get_pcm_frame_count(mp3);
        if (rawCount > 0) {
            // Valid length. MP3 has no native sample-accurate seek, so build + bind a seek table for
            // fast, accurate seeks (the table is client memory that must outlive the decoder; freed in
            // freeSfx). Without it dr_mp3 brute-force seeks, which is slow and jumpy.
            a.pcmSamples = rawCount;
            drmp3_uint32 seekPointCount = 512;
            drmp3_seek_point *seekPoints = (drmp3_seek_point *)malloc(seekPointCount * sizeof(drmp3_seek_point));
            if (seekPoints && drmp3_calculate_seek_points(mp3, &seekPointCount, seekPoints) && seekPointCount > 0) {
                drmp3_bind_seek_table(mp3, seekPointCount, seekPoints);
                a.mp3SeekPoints = seekPoints;
            } else {
                free(seekPoints);
            }
        } else {
            // Unknown length even after a scan (very unusual). Decode-count for a usable duration and
            // leave the seek table unbuilt so seeking falls back to dr_mp3's brute force.
            a.pcmSamples = mp3FrameCountByScan(mp3);
        }

        drmp3_seek_to_pcm_frame(mp3, 0);                             // counting/seek-table scan moved the head; rewind
        readMp3Title(&a, fd, fsz);
    } else if (isFlac) {
        // flac stream: dr_flac decodes from the compressed bytes (kept in flacData) on demand. The
        // total length is in the flac header, so this opens instantly (no scan).
        drflac *flac = drflac_open_memory_with_metadata(fd, fsz, onFlacMeta, &a, NULL);
        if (!flac) { free(fd); return a; }
        a.flac       = flac;
        a.flacData   = fd;                                           // dr_flac references this; keep it alive
        a.sampleRate = (int)flac->sampleRate;
        a.channels   = (int)flac->channels;
        a.pcmSamples = (uint32_t)flac->totalPCMFrameCount;
    } else if (a.isOgg) {
        int err;
        stb_vorbis *v = stb_vorbis_open_memory(fd, (int)fsz, &err, NULL);
        if (!v) { free(fd); return a; }

        stb_vorbis_info info = stb_vorbis_get_info(v);
        a.vorbis = v;
        a.vorbisFileData = fd;
        a.vorbisFileSize = fsz;
        a.sampleRate = info.sample_rate;
        a.channels = info.channels;
        a.pcmSamples = stb_vorbis_stream_length_in_samples(v);   // total length, for the seek bar / duration
        readOggTitle(&a);
    } else {
        // a wav that didn't open via the streaming path (openWavStream): decode it to memory instead.
        loadPcmToMemory(fd, fsz, 0, &a);
    }

    return a;
}

// dr_wav IO callbacks backed by a cellFs file descriptor (passed through as pUserData).
static size_t wavRead(void *user, void *out, size_t bytes) {
    uint64_t got = 0;
    cellFsRead((int)(intptr_t)user, out, bytes, &got);
    return (size_t)got;
}
static drwav_bool32 wavSeek(void *user, int offset, drwav_seek_origin origin) {
    uint64_t pos;
    int whence = (origin == DRWAV_SEEK_CUR) ? CELL_FS_SEEK_CUR : CELL_FS_SEEK_SET;
    return cellFsLseek((int)(intptr_t)user, offset, whence, &pos) == CELL_FS_SUCCEEDED;
}
static drwav_bool32 wavTell(void *user, drwav_int64 *cursor) {
    uint64_t pos = 0;
    if (cellFsLseek((int)(intptr_t)user, 0, CELL_FS_SEEK_CUR, &pos) != CELL_FS_SUCCEEDED) return 0;
    *cursor = (drwav_int64)pos;
    return 1;
}

// Opens a wav for disk streaming via dr_wav with cellFs-backed callbacks: only the header is read
// here, so even an hour-long file opens instantly and is never loaded into memory. Returns 0 on
// success (a is a ready SFX_STREAM handle), -1 if the file isn't a wav dr_wav can open (fd closed).
static int openWavStream(const char *path, Audio *a) {
    memset(a, 0, sizeof(*a));
    a->volume = SFX_DEFAULT_VOLUME;
    a->speed  = SFX_DEFAULT_SPEED;
    a->loop   = SFX_LOOP;
    a->state  = SFX_STATE_STOPPED;
    a->mode   = SFX_STREAM;
    a->seekRequest = -1;

    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return -1;

    drwav *wav = (drwav *)malloc(sizeof(drwav));
    if (!wav || !drwav_init(wav, wavRead, wavSeek, wavTell, (void *)(intptr_t)fd, NULL)) {
        if (wav) free(wav);
        cellFsClose(fd);
        return -1;
    }
    a->wav        = wav;
    a->wavFd      = fd;
    a->channels   = wav->channels;
    a->sampleRate = (int)wav->sampleRate;
    a->pcmSamples = (uint32_t)wav->totalPCMFrameCount;
    return 0;
}

Audio loadSfx(const char *path, SfxMode mode) {
    // a streamed wav is read straight from disk via dr_wav (no full-file load); everything else falls
    // through to the in-memory path below, which holds the compressed bytes and decodes on demand.
    if (mode == SFX_STREAM) {
        Audio a;
        if (openWavStream(path, &a) == 0) return a;
    }
    uint32_t fsz = 0;
    uint8_t *fd = readFileAlloc(path, &fsz);
    return loadSfxBuffer(fd, fsz, mode);
}

// Loads from an in-memory WAV/OGG blob (copies what it needs; caller may free `data`).
Audio loadSfxMem(const void *data, uint32_t size, SfxMode mode) {
    uint8_t *fd = NULL;
    if (data && size) { fd = (uint8_t *)malloc(size); if (fd) memcpy(fd, data, size); else size = 0; }
    return loadSfxBuffer(fd, size, mode);
}

void freeSfx(Audio *a) {
    stopSfx(a);
    if (a->pcmData)        { free(a->pcmData);        a->pcmData = NULL; }
    if (a->vorbis)         { stb_vorbis_close(a->vorbis); a->vorbis = NULL; }
    if (a->vorbisFileData) { free(a->vorbisFileData);  a->vorbisFileData = NULL; }
    if (a->mp3)            { drmp3_uninit((drmp3 *)a->mp3); free(a->mp3); a->mp3 = NULL; }
    if (a->mp3Data)        { free(a->mp3Data); a->mp3Data = NULL; }
    if (a->mp3SeekPoints)  { free(a->mp3SeekPoints); a->mp3SeekPoints = NULL; }
    if (a->flac)           { drflac_close((drflac *)a->flac); a->flac = NULL; }   // drflac_close frees the handle
    if (a->flacData)       { free(a->flacData); a->flacData = NULL; }
    if (a->wav)            { drwav_uninit((drwav *)a->wav); free(a->wav); a->wav = NULL; cellFsClose(a->wavFd); a->wavFd = 0; }
}

// ============================================================================
// playback control
// ============================================================================

void playSfx(Audio *a, float volume, float speed, int loop) {
    a->volume = volume;
    a->speed  = speed;
    a->loop   = loop;
    a->playPos = 0.0;
    a->seekRequest = -1;        // drop any stale seek from a previous play
    a->srcCarryFrames = 0;      // discard any leftover resample carry from a previous play
    a->srcCarryPos = 0.0;
    a->fadeTarget = volume;     // start with no fade in progress
    a->fadeStep   = 0.0f;
    a->state  = SFX_STATE_PLAYING;

    if (a->mode == SFX_STREAM) {
        // rewind the decoder to the start (vorbis has a dedicated reset; the rest seek to frame 0)
        if (a->vorbis) stb_vorbis_seek_start(a->vorbis);
        else           seekStream(a, 0);
    }

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

    // The stream is out of the mix list now, but the mixer thread may be mid-block on it. Wait out
    // that block so a following freeSfx can't free a decoder the mixer is still reading. Bounded:
    // the mixer clears mixingStream after every stream, so this never spins beyond one block.
    __sync_synchronize();
    while (mixingStream == a) sys_timer_usleep(100);
}

void fadeSfx(Audio *a, float target, float seconds) {
    if (!a) return;
    a->fadeTarget = target;
    if (seconds <= 0.0f) {                       // instant
        a->volume = target;
        a->fadeStep = 0.0f;
        if (target <= 0.0f) a->state = SFX_STATE_STOPPED;
        return;
    }
    float blocks = seconds * ((float)SFX_SAMPLE_RATE / (float)SFX_BLOCK_SAMPLES);
    a->fadeStep = (target - a->volume) / (blocks > 1.0f ? blocks : 1.0f);
}

void seekSfx(Audio *a, float seconds) {
    if (!a) return;
    if (seconds < 0.0f) seconds = 0.0f;

    if (a->mode == SFX_MEMORY && a->pcmData && a->pcmSamples) {
        // memory clip: the mixer only reads playPos, so writing it here is a benign single-field race.
        double pos = (double)seconds * (double)a->sampleRate;
        if (pos > (double)(a->pcmSamples - 1)) pos = (double)(a->pcmSamples - 1);
        a->playPos = pos;
    } else if (a->mode == SFX_STREAM && (a->vorbis || a->mp3 || a->flac || a->wav)) {
        // stream: hand the target frame to the mixer thread, which owns the decoder / file reads.
        long target = (long)(seconds * (float)a->sampleRate);
        if (a->pcmSamples && target > (long)a->pcmSamples - 1) target = (long)a->pcmSamples - 1;
        if (target < 0) target = 0;
        a->seekRequest = (int)target;
    }
}

int getSfxWaveform(const Audio *a, float *out, int maxBins) {
    if (!a || !out || maxBins <= 0) return 0;
    int n = maxBins < SFX_VIZ_BINS ? maxBins : SFX_VIZ_BINS;
    for (int i = 0; i < n; i++) out[i] = a->vizPeaks[i];
    return n;
}

void setSfxVolume(Audio *a, float volume) {
    if (!a) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    a->volume     = volume;
    a->fadeTarget = volume;   // cancel any fade in progress so it doesn't override this
    a->fadeStep   = 0.0f;
}

float getSfxPositionSeconds(const Audio *a) {
    if (!a || a->sampleRate == 0) return 0.0f;
    return (float)(a->playPos / (double)a->sampleRate);
}

float getSfxDurationSeconds(const Audio *a) {
    if (!a || a->sampleRate == 0) return 0.0f;
    return (float)a->pcmSamples / (float)a->sampleRate;
}

int isPlayableAudioFile(const char *name) {
    const char *ext = getExtension(name);
    if (!ext) return 0;
    return strCmpICase(ext, "wav") == 0 || strCmpICase(ext, "ogg") == 0 ||
           strCmpICase(ext, "mp3") == 0 || strCmpICase(ext, "flac") == 0;
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
