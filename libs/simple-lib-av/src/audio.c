// audio - multi-stream audio mixer: wav and ogg/vorbis, plus mp3 and flac via vendored decoders
#include "audio.h"
#include "vfs.h"               // getExtension
#include "string-utilities.h"   // strCmpICase
#include "thread.h"
#include <cell/audio.h>
#include <cell/sysmodule.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <string.h>
#include <stdlib.h>

#define AUDIO_MAX_STREAMS    8
#define AUDIO_SAMPLE_RATE    48000
#define AUDIO_BLOCK_SAMPLES  CELL_AUDIO_BLOCK_SAMPLES
#define AUDIO_PORT_BLOCKS    8     // audio-port ring-buffer block count (cellAudioPortOpen nBlock)

// reads entire file into a malloc'd buffer. caller frees. NULL on failure.
static uint8_t *readFileAlloc(const char *path, uint32_t *outSize)
{
   VfsStat st;
   if (statPath(path, &st) != 0) return NULL;
   uint32_t size = (uint32_t)st.size;
   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return NULL;
   uint8_t *buf = (uint8_t *)malloc(size);
   if (!buf) { closeFs(&file); return NULL; }
   uint64_t totalRead = 0;
   while (totalRead < size) {
      int64_t r = readFs(&file, buf + totalRead, size - totalRead);
      if (r <= 0) break;
      totalRead += (uint64_t)r;
   }
   closeFs(&file);
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

static Audio *streams[AUDIO_MAX_STREAMS];
static int streamCount = 0;
static float masterVolume = 1.0f;
static sys_ppu_thread_t audioThread;
static volatile int audioRunning = 0;
static CellAudioPortConfig portConfig;
static uint32_t portNum;

// The stream the mixer thread is currently reading from (NULL between streams). stopAudio waits on
// this before returning so a following freeAudio can't release a decoder mid-block. See stopAudio.
static Audio * volatile mixingStream = NULL;

// external PCM feed (video playback): lock-free single-producer ring drained by the mixer
#define FEED_RING_FRAMES  32768   // stereo frames, power of two (~0.68 s at 48 kHz)
#define FEED_PRIME_FRAMES 16384   // backlog required before the mixer starts draining (~0.35 s)
static struct {
   float   *ring;                 // interleaved stereo
   volatile uint32_t head, tail;  // free-running frame counters (producer / mixer)
   volatile uint64_t consumed;    // total source frames mixed out — the video player's A/V clock
   double   readPos;              // fractional resample position past `tail`
   int      sampleRate;
   float    volume;
   volatile int active, paused;
   volatile int priming;          // hold consumption until a backlog exists (start / post-flush)
   volatile int mixerInFeed;      // handshake so closeAudioPcmFeed never frees the ring mid-block
} feed;

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
   for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
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
static inline void appendAudioViz(Audio *a, float peak) {
   memmove(a->vizPeaks, a->vizPeaks + 1, (AUDIO_VIZ_BINS - 1) * sizeof(float));
   a->vizPeaks[AUDIO_VIZ_BINS - 1] = peak;
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

// Applies a pending user seek (seekRequest, in source frames) on the mixer thread, which owns the
// decoder. No-op when none is pending. Resets the resample carry and syncs playPos to the target.
static void applyStreamSeek(Audio *a) {
   if (a->seekRequest < 0) return;
   seekStream(a, (uint32_t)a->seekRequest);
   a->playPos        = (double)a->seekRequest;
   a->seekRequest    = -1;
   a->srcCarryFrames = 0;
   a->srcCarryPos    = 0.0;
}

// one mixer block for a pull-decoder stream. The resample carry (decoded-but-unconsumed frames +
// fractional position) persists across blocks, so resampling stays phase-continuous instead of
// restarting (and dropping a couple of frames) every block.
static void mixStreamBlock(Audio *a, float *mix, double step, float vol) {
   int ch = streamChannels(a);

   applyStreamSeek(a);   // honor a pending user seek before decoding this block

   // top up the carry buffer so it covers this block's resample span (fractional pos + 256*step)
   int need = (int)(a->srcCarryPos + AUDIO_BLOCK_SAMPLES * step) + 2;
   if (need > AUDIO_STREAM_FRAMES) need = AUDIO_STREAM_FRAMES;

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
      appendAudioViz(a, blockPeak(a->srcCarry, consumed > 0 ? consumed : a->srcCarryFrames, ch));

      // keep the unconsumed tail + fractional position for the next block
      int remaining = a->srcCarryFrames - consumed;
      if (remaining > 0)
         memmove(a->srcCarry, a->srcCarry + consumed * ch, (size_t)remaining * ch * sizeof(float));
      a->srcCarryFrames = remaining;
      a->srcCarryPos = pos - consumed;
      a->playPos += consumed;
   }

   // EOF: stop once the carry is down to its last frame. mixSamples can't interpolate the final
   // frame (it needs frame p+1), so ~1 frame always remains at end-of-stream. Requiring exactly 0
   // left the stream stuck PLAYING at the end forever, so X toggled pause instead of restarting.
   // Dropping that one inaudible frame lets the stream reach STOPPED so a finished track replays.
   if (ended && !a->loop && a->srcCarryFrames <= 1) a->state = AUDIO_STATE_STOPPED;
}

// mixes one block from the external PCM feed, resampling with linear interpolation. Underrun just
// leaves silence for the rest of the block; the clock only advances by frames actually consumed.
static void mixFeedBlock(float *mix) {
   feed.mixerInFeed = 1;
   __sync_synchronize();
   if (!feed.active || feed.paused || !feed.ring) { feed.mixerInFeed = 0; return; }

   // hold consumption until the producer has built a backlog: draining from the very first pushed
   // frame underruns in bursts at startup, and each underrun freezes the A/V clock (video stutter)
   if (feed.priming) {
      if (feed.head - feed.tail < FEED_PRIME_FRAMES) { feed.mixerInFeed = 0; return; }
      feed.priming = 0;
   }

   double step = (double)feed.sampleRate / (double)AUDIO_SAMPLE_RATE;
   float vol = feed.volume * masterVolume;
   uint32_t available = feed.head - feed.tail;

   double pos = feed.readPos;
   for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
      uint32_t base = (uint32_t)pos;
      if (base + 1 >= available) break;   // underrun: need a frame pair to interpolate
      float fraction = (float)(pos - base);
      uint32_t i0 = ((feed.tail + base)     & (FEED_RING_FRAMES - 1)) * 2;
      uint32_t i1 = ((feed.tail + base + 1) & (FEED_RING_FRAMES - 1)) * 2;
      mix[i * 2]     += vol * (feed.ring[i0]     + fraction * (feed.ring[i1]     - feed.ring[i0]));
      mix[i * 2 + 1] += vol * (feed.ring[i0 + 1] + fraction * (feed.ring[i1 + 1] - feed.ring[i0 + 1]));
      pos += step;
   }

   uint32_t consumedFrames = (uint32_t)pos;
   feed.readPos = pos - consumedFrames;
   feed.tail += consumedFrames;
   feed.consumed += consumedFrames;
   __sync_synchronize();
   feed.mixerInFeed = 0;
}

static void audioMixerThread(uint64_t arg) {
   (void)arg;
   float mix[AUDIO_BLOCK_SAMPLES * 2];
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

         // Claim the stream BEFORE re-reading its state. Paired with the fence in stopAudio this
         // is a mutual-flag handshake: stopAudio either sees this claim and waits, or its STOPPED
         // store is visible just below and we bail -- so we never touch a decoder being freed.
         mixingStream = a;
         __sync_synchronize();
         if (a->state != AUDIO_STATE_PLAYING) {
            applyStreamSeek(a);   // honor a seek made while paused/stopped (mixer owns decoder)
            mixingStream = NULL;
            continue;
         }

         // advance a volume fade by one block, if one is running
         if (a->fadeStep != 0.0f) {
            a->volume += a->fadeStep;
            if ((a->fadeStep > 0.0f && a->volume >= a->fadeTarget) ||
                (a->fadeStep < 0.0f && a->volume <= a->fadeTarget)) {
               a->volume = a->fadeTarget;
               a->fadeStep = 0.0f;
               if (a->fadeTarget <= 0.0f) a->state = AUDIO_STATE_STOPPED;   // faded out
            }
         }

         float vol = a->volume * masterVolume;
         double step = (double)a->sampleRate / (double)AUDIO_SAMPLE_RATE * a->speed;

         if (a->mode == AUDIO_MEMORY && a->pcmData) {
            double startPos = a->playPos;
            double pos = a->playPos;
            mixSamples(a->pcmData, a->pcmSamples, a->channels, mix, &pos, step, vol);

            int frames = (int)(pos - startPos);
            if (frames > 0)
               appendAudioViz(a, blockPeak(a->pcmData + (uint32_t)startPos * a->channels, frames, a->channels));

            if ((uint32_t)pos + 1 >= a->pcmSamples) {
               if (a->loop) { a->playPos = 0.0; }
               else { a->state = AUDIO_STATE_STOPPED; }
            } else {
               a->playPos = pos;
            }
         } else if (a->mode == AUDIO_STREAM && (a->vorbis || a->mp3 || a->flac || a->wav)) {
            mixStreamBlock(a, mix, step, vol);
         }

         __sync_synchronize();
         mixingStream = NULL;   // released; safe for freeAudio to tear this stream down now
      }

      mixFeedBlock(mix);

      for (int i = 0; i < AUDIO_BLOCK_SAMPLES * 2; i++) {
         if (mix[i] > 1.0f) mix[i] = 1.0f;
         else if (mix[i] < -1.0f) mix[i] = -1.0f;
      }

      uint32_t writeBlock = (curBlock + 1) % AUDIO_PORT_BLOCKS;
      float *dst = (float *)(uintptr_t)(portConfig.portAddr +
                   AUDIO_BLOCK_SAMPLES * 2 * sizeof(float) * writeBlock);
      memcpy(dst, mix, AUDIO_BLOCK_SAMPLES * 2 * sizeof(float));
   }

   sys_ppu_thread_exit(0);
}

// ============================================================================
// init / term
// ============================================================================

int initAudio(void) {
   cellSysmoduleLoadModule(CELL_SYSMODULE_AUDIO);

   CellAudioPortParam p;
   memset(&p, 0, sizeof(p));
   p.nChannel = 2;
   p.nBlock = AUDIO_PORT_BLOCKS;
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

void termAudio(void) {
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

   if (sr != AUDIO_SAMPLE_RATE) {
      uint32_t rn;
      float *rp = resamplePcm(pcm, ch, sr, AUDIO_SAMPLE_RATE, nf, &rn);
      free(pcm);
      if (!rp) return;
      pcm = rp; nf = rn; sr = AUDIO_SAMPLE_RATE;
   }
   a->pcmData    = pcm;
   a->pcmSamples = nf;
   a->sampleRate = sr;
   a->channels   = ch;
   a->mode       = AUDIO_MEMORY;
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
// Shared by loadAudio (reads a file) and loadAudioMem (already-in-memory blob).
static Audio loadAudioBuffer(uint8_t *fd, uint32_t fsz, AudioMode mode) {
   Audio a;
   memset(&a, 0, sizeof(a));
   a.volume = AUDIO_DEFAULT_VOLUME;
   a.speed  = AUDIO_DEFAULT_SPEED;
   a.loop   = AUDIO_NO_LOOP;
   a.state  = AUDIO_STATE_STOPPED;
   a.mode   = mode;
   a.seekRequest = -1;

   if (!fd || fsz < 4) { if (fd) free(fd); return a; }

   a.isOgg = (fd[0] == 'O' && fd[1] == 'g' && fd[2] == 'g' && fd[3] == 'S');
   int isMp3 = (fd[0] == 'I' && fd[1] == 'D' && fd[2] == '3') ||      // ID3v2 tag
            (fd[0] == 0xFF && (fd[1] & 0xE0) == 0xE0);             // raw MPEG frame sync
   int isFlac = (fd[0] == 'f' && fd[1] == 'L' && fd[2] == 'a' && fd[3] == 'C');

   if (mode == AUDIO_MEMORY) {
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
         // freeAudio). Without it dr_mp3 brute-force seeks, which is slow and jumpy.
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

// dr_wav IO callbacks backed by an open VfsFile* (passed through as pUserData).
static size_t wavRead(void *user, void *out, size_t bytes) {
   int64_t got = readFs((VfsFile *)user, out, bytes);
   return got > 0 ? (size_t)got : 0;
}
static drwav_bool32 wavSeek(void *user, int offset, drwav_seek_origin origin) {
   // dr_wav (>= v0.14) seeks from the end to validate the data-chunk size against the real file
   // length. All three origins must be mapped -- treating a from-end seek as from-start makes the
   // file look 0 bytes long, so dr_wav reports a garbage frame count (a ~49-hour duration here).
   int whence = (origin == DRWAV_SEEK_CUR) ? VFS_SEEK_CUR
            : (origin == DRWAV_SEEK_END) ? VFS_SEEK_END
            : VFS_SEEK_SET;
   return seekFs((VfsFile *)user, offset, whence) >= 0;
}
static drwav_bool32 wavTell(void *user, drwav_int64 *cursor) {
   int64_t pos = seekFs((VfsFile *)user, 0, VFS_SEEK_CUR);
   if (pos < 0) return 0;
   *cursor = (drwav_int64)pos;
   return 1;
}

// Opens a wav for disk streaming via dr_wav with VFS-backed callbacks: only the header is read
// here, so even an hour-long file opens instantly and is never loaded into memory. Returns 0 on
// success (a is a ready AUDIO_STREAM handle), -1 if the file isn't a wav dr_wav can open (fd closed).
static int openWavStream(const char *path, Audio *a) {
   memset(a, 0, sizeof(*a));
   a->volume = AUDIO_DEFAULT_VOLUME;
   a->speed  = AUDIO_DEFAULT_SPEED;
   a->loop   = AUDIO_NO_LOOP;
   a->state  = AUDIO_STATE_STOPPED;
   a->mode   = AUDIO_STREAM;
   a->seekRequest = -1;

   VfsFile *file = (VfsFile *)malloc(sizeof(VfsFile));
   if (!file || openFs(path, VFS_O_RDONLY, file) != 0) {
      if (file) free(file);
      return -1;
   }

   drwav *wav = (drwav *)malloc(sizeof(drwav));
   if (!wav || !drwav_init(wav, wavRead, wavSeek, wavTell, file, NULL)) {
      if (wav) free(wav);
      closeFs(file);
      free(file);
      return -1;
   }
   a->wav        = wav;
   a->wavFile    = file;
   a->channels   = wav->channels;
   a->sampleRate = (int)wav->sampleRate;
   a->pcmSamples = (uint32_t)wav->totalPCMFrameCount;
   return 0;
}

Audio loadAudio(const char *path, AudioMode mode) {
   // a streamed wav is read straight from disk via dr_wav (no full-file load); everything else falls
   // through to the in-memory path below, which holds the compressed bytes and decodes on demand.
   if (mode == AUDIO_STREAM) {
      Audio a;
      if (openWavStream(path, &a) == 0) return a;
   }
   uint32_t fsz = 0;
   uint8_t *fd = readFileAlloc(path, &fsz);
   return loadAudioBuffer(fd, fsz, mode);
}

// Loads from an in-memory WAV/OGG blob (copies what it needs; caller may free `data`).
Audio loadAudioMem(const void *data, uint32_t size, AudioMode mode) {
   uint8_t *fd = NULL;
   if (data && size) { fd = (uint8_t *)malloc(size); if (fd) memcpy(fd, data, size); else size = 0; }
   return loadAudioBuffer(fd, size, mode);
}

void freeAudio(Audio *a) {
   stopAudio(a);
   if (a->pcmData)        { free(a->pcmData);        a->pcmData = NULL; }
   if (a->vorbis)         { stb_vorbis_close(a->vorbis); a->vorbis = NULL; }
   if (a->vorbisFileData) { free(a->vorbisFileData);  a->vorbisFileData = NULL; }
   if (a->mp3)            { drmp3_uninit((drmp3 *)a->mp3); free(a->mp3); a->mp3 = NULL; }
   if (a->mp3Data)        { free(a->mp3Data); a->mp3Data = NULL; }
   if (a->mp3SeekPoints)  { free(a->mp3SeekPoints); a->mp3SeekPoints = NULL; }
   if (a->flac)           { drflac_close((drflac *)a->flac); a->flac = NULL; }   // drflac_close frees the handle
   if (a->flacData)       { free(a->flacData); a->flacData = NULL; }
   if (a->wav)            { drwav_uninit((drwav *)a->wav); free(a->wav); a->wav = NULL; }
   if (a->wavFile)        { closeFs((VfsFile *)a->wavFile); free(a->wavFile); a->wavFile = NULL; }
}

// ============================================================================
// playback control
// ============================================================================

void playAudio(Audio *a, float volume, float speed, int loop) {
   a->volume = volume;
   a->speed  = speed;
   a->loop   = loop;
   a->playPos = 0.0;
   a->seekRequest = -1;        // drop any stale seek from a previous play
   a->srcCarryFrames = 0;      // discard any leftover resample carry from a previous play
   a->srcCarryPos = 0.0;
   a->fadeTarget = volume;     // start with no fade in progress
   a->fadeStep   = 0.0f;
   a->seekRequest = -1;        // clear before the wait so the mixer can't apply a stale seek below

   // A replay restarts a stream that is still in the mix list. Wait out any block the mixer is
   // running on it, then rewind the decoder and go live LAST -- otherwise the mixer could decode
   // (a slow disk-backed wav especially) while this rewind is mid-seek, leaving the replay silent.
   __sync_synchronize();
   while (mixingStream == a) sys_timer_usleep(100);

   if (a->mode == AUDIO_STREAM) {
      if (a->vorbis) stb_vorbis_seek_start(a->vorbis);
      else           seekStream(a, 0);
   }

   int present = 0;
   for (int i = 0; i < streamCount; i++)
      if (streams[i] == a) { present = 1; break; }
   if (!present && streamCount < AUDIO_MAX_STREAMS)
      streams[streamCount++] = a;

   __sync_synchronize();
   a->state = AUDIO_STATE_PLAYING;   // live only once the decoder is rewound and we are registered
}

void stopAudio(Audio *a) {
   a->state = AUDIO_STATE_STOPPED;
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
   // that block so a following freeAudio can't free a decoder the mixer is still reading. Bounded:
   // the mixer clears mixingStream after every stream, so this never spins beyond one block.
   __sync_synchronize();
   while (mixingStream == a) sys_timer_usleep(100);
}

void fadeAudio(Audio *a, float target, float seconds) {
   if (!a) return;
   a->fadeTarget = target;
   if (seconds <= 0.0f) {                       // instant
      a->volume = target;
      a->fadeStep = 0.0f;
      if (target <= 0.0f) a->state = AUDIO_STATE_STOPPED;
      return;
   }
   float blocks = seconds * ((float)AUDIO_SAMPLE_RATE / (float)AUDIO_BLOCK_SAMPLES);
   a->fadeStep = (target - a->volume) / (blocks > 1.0f ? blocks : 1.0f);
}

void seekAudio(Audio *a, float seconds) {
   if (!a) return;
   if (seconds < 0.0f) seconds = 0.0f;

   if (a->mode == AUDIO_MEMORY && a->pcmData && a->pcmSamples) {
      // memory clip: the mixer only reads playPos, so writing it here is a benign single-field race.
      double pos = (double)seconds * (double)a->sampleRate;
      if (pos > (double)(a->pcmSamples - 1)) pos = (double)(a->pcmSamples - 1);
      a->playPos = pos;
   } else if (a->mode == AUDIO_STREAM && (a->vorbis || a->mp3 || a->flac || a->wav)) {
      // stream: hand the target frame to the mixer thread, which owns the decoder / file reads.
      // Clamp in double -- pcmSamples is uint32, so a (long) cast can go negative on PS3 (32-bit
      // long) for very large counts and collapse every seek to 0.
      double target = (double)seconds * (double)a->sampleRate;
      if (target < 0.0) target = 0.0;
      if (a->pcmSamples && target > (double)(a->pcmSamples - 1)) target = (double)(a->pcmSamples - 1);
      a->seekRequest = (int)target;
   }
}

int getAudioWaveform(const Audio *a, float *out, int maxBins) {
   if (!a || !out || maxBins <= 0) return 0;
   int n = maxBins < AUDIO_VIZ_BINS ? maxBins : AUDIO_VIZ_BINS;
   for (int i = 0; i < n; i++) out[i] = a->vizPeaks[i];
   return n;
}

void setAudioVolume(Audio *a, float volume) {
   if (!a) return;
   if (volume < 0.0f) volume = 0.0f;
   if (volume > 1.0f) volume = 1.0f;
   a->volume     = volume;
   a->fadeTarget = volume;   // cancel any fade in progress so it doesn't override this
   a->fadeStep   = 0.0f;
}

float getAudioPositionSeconds(const Audio *a) {
   if (!a || a->sampleRate == 0) return 0.0f;
   return (float)(a->playPos / (double)a->sampleRate);
}

float getAudioDurationSeconds(const Audio *a) {
   if (!a || a->sampleRate == 0) return 0.0f;
   return (float)a->pcmSamples / (float)a->sampleRate;
}

int isPlayableAudioFile(const char *name) {
   const char *ext = getExtension(name);
   if (!ext) return 0;
   return strCmpICase(ext, "wav") == 0 || strCmpICase(ext, "ogg") == 0 ||
         strCmpICase(ext, "mp3") == 0 || strCmpICase(ext, "flac") == 0;
}

void pauseAudio(Audio *a) {
   if (a->state == AUDIO_STATE_PLAYING)
      a->state = AUDIO_STATE_PAUSED;
}

void resumeAudio(Audio *a) {
   if (a->state == AUDIO_STATE_PAUSED)
      a->state = AUDIO_STATE_PLAYING;
}

void setAudioMasterVolume(float vol) {
   if (vol < 0.0f) vol = 0.0f;
   if (vol > 1.0f) vol = 1.0f;
   masterVolume = vol;
}

void raiseAudioMasterVolume(float amount) {
   setAudioMasterVolume(masterVolume + amount);
}

void lowerAudioMasterVolume(float amount) {
   setAudioMasterVolume(masterVolume - amount);
}

// ============================================================================
// external PCM feed (video playback)
// ============================================================================

int openAudioPcmFeed(int sampleRate) {
   if (feed.active || sampleRate <= 0) return -1;
   feed.ring = (float *)calloc(FEED_RING_FRAMES * 2, sizeof(float));
   if (!feed.ring) return -1;
   feed.head = feed.tail = 0;
   feed.consumed = 0;
   feed.readPos = 0.0;
   feed.sampleRate = sampleRate;
   feed.volume = 1.0f;
   feed.paused = 0;
   feed.priming = 1;
   __sync_synchronize();
   feed.active = 1;
   return 0;
}

void closeAudioPcmFeed(void) {
   if (!feed.active) return;
   feed.active = 0;
   __sync_synchronize();
   while (feed.mixerInFeed) sys_timer_usleep(200);   // let a mixer block in flight leave the ring
   free(feed.ring);
   feed.ring = NULL;
}

int pushAudioPcm(const float *stereoFrames, int frameCount) {
   if (!feed.active || frameCount <= 0) return 0;
   uint32_t space = FEED_RING_FRAMES - (feed.head - feed.tail);
   if ((uint32_t)frameCount > space) frameCount = (int)space;
   for (int i = 0; i < frameCount; i++) {
      uint32_t slot = ((feed.head + i) & (FEED_RING_FRAMES - 1)) * 2;
      feed.ring[slot]     = stereoFrames[i * 2];
      feed.ring[slot + 1] = stereoFrames[i * 2 + 1];
   }
   __sync_synchronize();   // frames must be visible before the head moves
   feed.head += frameCount;
   return frameCount;
}

int getAudioPcmFeedSpace(void) {
   if (!feed.active) return 0;
   return (int)(FEED_RING_FRAMES - (feed.head - feed.tail));
}

uint64_t getAudioPcmFeedPlayedFrames(void) {
   return feed.consumed;
}

void setAudioPcmFeedVolume(float volume) {
   if (volume < 0.0f) volume = 0.0f;
   if (volume > 1.0f) volume = 1.0f;
   feed.volume = volume;
}

void pauseAudioPcmFeed(int paused) {
   feed.paused = paused;
}

void flushAudioPcmFeed(void) {
   if (!feed.active) return;
   int wasPaused = feed.paused;
   feed.paused = 1;
   __sync_synchronize();
   while (feed.mixerInFeed) sys_timer_usleep(200);   // let a mixer block in flight leave the ring
   feed.tail = feed.head;
   feed.readPos = 0.0;
   feed.consumed = 0;   // the A/V clock re-anchors at the first frame pushed after the flush
   feed.priming = 1;    // rebuild the backlog before draining resumes (same as at open)
   __sync_synchronize();
   feed.paused = wasPaused;
}
