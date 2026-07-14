// video-player - streaming playback engine. See video-player.h.
//
// A background thread demuxes + decodes ahead into a ring of frame slots. The UI thread pulls the
// due frame each render via getVideoFrame. A slot moves EMPTY -> FILLING (claimed by the decode
// thread) -> READY (decoded, awaiting its presentation time) -> DISPLAYING (handed to the UI) ->
// RETIRING (replaced, held one extra present while the RSX may still sample it) -> EMPTY. The decode
// thread only ever claims EMPTY slots, so a frame the GPU can see is never overwritten under it.
#include "video-player.h"
#include "demux.h"
#include "h264.h"
#include "decode-h264.h"
#include "decode-aac.h"
#include "audio.h"
#include "thread.h"
#include "dbg.h"
#include <stdlib.h>
#include <string.h>
#include <sys/sys_time.h>

// the decode thread runs ahead into this ring; the extra slots past PREROLL_FRAMES are cushion the
// decoder banks during easy scenes and spends on hard (high-bitrate) frames without the picture stalling.
#define FRAME_SLOTS           8
#define PREROLL_FRAMES        4      // frames buffered before the first present (start latency, not ring depth)
#define PREROLL_AUDIO_TIMEOUT_US (5 * 1000 * 1000)   // give up waiting for audio to anchor the clock, play video alone
#define DECODE_STALL_MS       3000   // no AU consumed and no picture produced for this long = hard error
#define END_OF_STREAM_DRAIN_MS 500   // how long to wait for the last pictures after the final AU

// RETIRING holds a replaced frame for one extra present: the RSX may still be sampling it for the
// frame in flight, so the decoder must not refill it until the following present.
enum { SLOT_EMPTY, SLOT_FILLING, SLOT_READY, SLOT_DISPLAYING, SLOT_RETIRING };

typedef struct {
   uint8_t *yuv;        // YUV 4:2:0 planar, RSX-visible when the UI supplied the allocator
   uint64_t pts;        // nanoseconds
   int      width, height;
   int      state;
} FrameSlot;

struct VideoPlayer {
   VideoDemuxer demuxer;
   H264Decoder *decoder;
   int          allocW, allocH;   // coded frame-buffer dimensions (macroblock-aligned)
   VideoFrameFreeFn freeFrame;    // releases the slot buffers (matches the allocator used)

   VideoAu      pendingAu;        // demuxed AU not yet accepted by the decoder (queues were full)
   int          hasPendingAu;

   FrameSlot     slots[FRAME_SLOTS];
   sys_lwmutex_t lock;

   sys_ppu_thread_t thread;
   int              threadActive;
   volatile int     running;      // decode thread runs while set
   volatile int     ended;        // demux hit end of stream

   // audio pipeline (absent for files without an AAC track: video plays silent)
   AacDecoder      *aacDecoder;
   VideoDemuxer     audioDemuxer;       // separate audio-only source for split (adaptive) streams
   int              hasAudioDemuxer;    // audioDemuxer is open and supplies the audio (else it's in `demuxer`)
   sys_ppu_thread_t audioThread;
   int              audioThreadActive;
   int              audioFeedOpen;      // mixer external-PCM feed is ours to close
   uint8_t         *adtsBuffers[2];     // rotating ADTS frame scratch (7-byte header + raw AAC frame)
   float           *pcmScratch;         // one decoded frame as stereo, en route to the mixer feed

   // audio-master clock: video paces against samples the mixer actually consumed, so A/V can't drift
   volatile int     audioAlive;         // audio thread is running (cleared on exit/error)
   volatile int     audioClockValid;    // audioClockBasePts anchored by the first decoded frame
   uint64_t         audioClockBasePts;  // pts (ns) of the first PCM frame handed to the mixer feed

   // seek: the UI posts a target; the decode thread performs it (parking the audio thread while both
   // pipelines flush), then decode restarts from the nearest earlier cued keyframe
   volatile int64_t seekRequestNs;      // pending target in ns, -1 = none
   int              seekLandAfter;      // 1 = land on the next keyframe at/after the target (sponsor skip), not before
   volatile int     audioFlushRequest;  // decode thread -> audio thread: flush and park
   volatile int     audioFlushDone;     // audio thread -> decode thread: flushed, parked
   volatile int64_t audioSeekPendingNs; // decode thread -> audio thread: seek your demuxer here (-1 = none); keeps
                                        // the audio stream's network round trips off the picture's critical path

   // presentation clock (wall-time based until audio drives it)
   int      started;              // 1 once the first frame's pts anchored the clock
   uint64_t basePts;              // pts of the first presented frame (ns)
   uint64_t baseWallUs;           // wall time the clock was anchored (us)
   int      paused;
   uint64_t pauseWallUs;
   uint64_t currentPts;           // pts of the frame currently displayed
   int      displayIndex;         // slot being displayed, or -1
   uint64_t audioWaitStartUs;     // when pre-roll first started waiting on the audio clock (0 = not yet)
};

// defined below the decode thread but used by performSeek's audio-revive path
static VideoDemuxer *getAudioSource(VideoPlayer *player);
static void audioDecodeThread(uint64_t arg);

// ============================================================================
// decode thread
// ============================================================================

// claims a free slot for the decode thread to fill; returns its index or -1 if the ring is full.
static int claimEmptySlot(VideoPlayer *player)
{
   int found = -1;
   lock(&player->lock);
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_EMPTY) { player->slots[i].state = SLOT_FILLING; found = i; break; }
   unlock(&player->lock);
   return found;
}

// decodes the next presentation-order frame into `yuv`. returns 1 with dims+pts, 0 at end of
// stream, -1 on error. Feeds access units until the decoder emits a picture, always pulling pictures
// while an AU is outstanding — the decoder can't consume input while its frame buffers are full.
static int decodeNextFrame(VideoPlayer *player, uint8_t *yuv, int *width, int *height, uint64_t *pts, int *startedAtKeyframe)
{
   int stalledMs = 0;
   while (stalledMs < DECODE_STALL_MS && player->running && player->seekRequestNs < 0) {
      int got = getFrameH264(player->decoder, yuv, width, height, pts);
      if (got == 1) return 1;
      if (got < 0) return -1;

      // the demuxer rotates AU_BUFFER_COUNT buffers; keep the decoder's command queue as full as they
      // allow (the SDK notes throughput improves when the queue never runs dry) without overwriting
      // an AU the decoder is still reading
      if (!player->hasPendingAu && getAuBacklogH264(player->decoder) >= AU_BUFFER_COUNT) { sleepMs(1); stalledMs++; continue; }

      if (!player->hasPendingAu) {
         int r = readVideoAu(&player->demuxer, &player->pendingAu);
         if (r < 0) return -1;
         if (r == 0) {   // no more input: drain the pictures still in flight
            for (int waited = 0; waited < END_OF_STREAM_DRAIN_MS && player->running; waited++) {
               got = getFrameH264(player->decoder, yuv, width, height, pts);
               if (got != 0) return got;
               sleepMs(1);
            }
            return 0;
         }
         if (!*startedAtKeyframe) { if (!player->pendingAu.keyframe) continue; *startedAtKeyframe = 1; }
         player->hasPendingAu = 1;
      }

      int fed = decodeAuH264(player->decoder, player->pendingAu.data, player->pendingAu.size, player->pendingAu.pts);
      if (fed < 0) return -1;
      if (fed == 0) { player->hasPendingAu = 0; stalledMs = 0; continue; }
      sleepMs(1); stalledMs++;   // input queue full: give the decoder time, keep pulling pictures
   }
   if (player->running && stalledMs >= DECODE_STALL_MS) { logError("[video-player] decoder stalled\n"); return -1; }
   return 0;
}

// flushes both pipelines and jumps to the seek target (decode thread only). The frame on screen
// stays up until the new position's frames arrive.
static void performSeek(VideoPlayer *player)
{
   uint64_t targetNs = (uint64_t)player->seekRequestNs;

   // park the audio thread: it flushes its decoder + the mixer feed, then waits for our release
   if (player->audioAlive) {
      while (player->audioFlushDone && player->audioAlive) sleepMs(1);   // still leaving the previous park
      player->audioFlushRequest = 1;
      while (!player->audioFlushDone && player->audioAlive && player->running) sleepMs(1);
   }

   // flush the decoder; if the reset wedged it (EndSeq fatal) or it already died, rebuild it rather
   // than going dark (a NULL decoder makes decode report end-of-stream until a later seek revives it)
   if (!player->decoder || resetH264Decoder(player->decoder) != 0) {
      logWarn("[video-player] decoder reset failed, recreating decoder\n");
      destroyH264Decoder(player->decoder);
      player->decoder = createH264Decoder(player->allocW, player->allocH, player->demuxer.level, player->demuxer.h264.maxRefFrames);
   }
   player->hasPendingAu = 0;
   // land on the fragment keyframe covering the target - or, for a sponsor skip, the next keyframe at/after it
   uint64_t landedNs = seekVideoDemuxer(&player->demuxer, targetNs, player->seekLandAfter);
   // audio follows the video keyframe, but on its own thread: its demuxer seek costs a network
   // reconnect, and doing it here would hold up the first post-seek picture for no reason
   if (player->hasAudioDemuxer) player->audioSeekPendingNs = (int64_t)landedNs;
   logInfo("[video-player] seek to %ds landed at %ds\n", (int)(targetNs / 1000000000ull), (int)(landedNs / 1000000000ull));

   lock(&player->lock);
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_READY) player->slots[i].state = SLOT_EMPTY;
   player->started    = 0;               // the next ready frame re-anchors the clock
   player->ended      = 0;
   player->currentPts = landedNs;
   player->basePts    = landedNs;
   player->audioClockValid = 0;
   player->audioWaitStartUs = 0;         // fresh pre-roll: each seek gets the full window to wait for audio,
                                         // else a stale timer trips instantly and video starts on the wall clock
   unlock(&player->lock);

   // hold the audio feed until the first post-seek frame actually presents (getVideoFrame releases it), so
   // audio can't drain ahead and then race the video to catch up (the "speedup")
   if (player->audioFeedOpen) pauseAudioPcmFeed(1);

   // the audio thread self-exits at end-of-stream (audioAlive=0, but its decoder/feed/buffers stay intact). a
   // seek repositions the audio demuxer, so respawn the thread to consume the new position - otherwise once
   // the audio stream has ended once, playback stays silent for the rest of the video no matter how you seek.
   if (!player->audioAlive && player->audioThreadActive && player->aacDecoder && getAudioSource(player)->hasAudio) {
      joinThread(player->audioThread);        // reap the exited thread before reusing its handle
      resetAacDecoder(player->aacDecoder);
      flushAudioPcmFeed();                    // clean feed for the new position (stays paused from above)
      player->audioAlive = 1;                 // set before spawn, mirroring startAudioPipeline's ordering
      __sync_synchronize();
      player->audioThreadActive = (spawnJoinableThread(&player->audioThread, audioDecodeThread,
                                   (uint64_t)(uintptr_t)player, THREAD_PRIORITY_DEFAULT,
                                   THREAD_STACK_SIZE_64KB, "video-audio") == 0);
      if (!player->audioThreadActive) player->audioAlive = 0;
   }

   if (player->seekRequestNs == (int64_t)targetNs) player->seekRequestNs = -1;   // keep a newer request live
   __sync_synchronize();
   player->audioFlushRequest = 0;        // release the audio thread
}

static void decodeThread(uint64_t arg)
{
   VideoPlayer *player = (VideoPlayer *)(uintptr_t)arg;
   int startedAtKeyframe = 0;

   while (player->running) {
      if (player->seekRequestNs >= 0) { performSeek(player); startedAtKeyframe = 0; continue; }

      int slot = claimEmptySlot(player);
      if (slot < 0) { sleepMs(2); continue; }   // ring full: wait for the UI to consume a frame

      int width, height; uint64_t pts;
      int got = player->decoder ? decodeNextFrame(player, player->slots[slot].yuv, &width, &height, &pts, &startedAtKeyframe) : -1;

      lock(&player->lock);
      if (got == 1) {
         player->slots[slot].pts    = pts;
         player->slots[slot].width  = width;
         player->slots[slot].height = height;
         player->slots[slot].state  = SLOT_READY;
         unlock(&player->lock);
      } else {
         player->slots[slot].state = SLOT_EMPTY;
         if (player->seekRequestNs < 0) player->ended = 1;
         unlock(&player->lock);
         // idle at end of stream (or after a hard error) until a seek restarts decode
         while (player->running && player->ended && player->seekRequestNs < 0) sleepMs(10);
      }
   }
   exitThread();
}

// ============================================================================
// audio thread: demuxed AAC frames -> cellAdec -> the mixer's external PCM feed
// ============================================================================

// the demuxer carrying the audio: a separate audio-only source for split (adaptive) streams,
// otherwise the main demuxer (the AAC track muxed alongside the video).
static VideoDemuxer *getAudioSource(VideoPlayer *player)
{
   return player->hasAudioDemuxer ? &player->audioDemuxer : &player->demuxer;
}

static void audioDecodeThread(uint64_t arg)
{
   VideoPlayer *player = (VideoPlayer *)(uintptr_t)arg;   // audioAlive is set by startAudioPipeline before we spawn
   int adtsIndex = 0, hasPendingFrame = 0, pendingSize = 0;
   uint64_t pendingPts = 0;
   const uint8_t *pendingFrame = 0;

   while (player->running) {
      // a seek is in progress: flush our pipeline and park until the decode thread releases us
      if (player->audioFlushRequest) {
         resetAacDecoder(player->aacDecoder);
         flushAudioPcmFeed();
         hasPendingFrame = 0;
         player->audioFlushDone = 1;
         while (player->audioFlushRequest && player->running) sleepMs(1);
         player->audioFlushDone = 0;
         continue;
      }

      // reposition our demuxer after a seek (posted by performSeek): the network reconnect this costs
      // runs here, in parallel with the video's own fragment load + decode
      if (player->audioSeekPendingNs >= 0) {
         seekVideoDemuxer(&player->audioDemuxer, (uint64_t)player->audioSeekPendingNs, 0);
         player->audioSeekPendingNs = -1;
         continue;
      }

      // drain decoded PCM into the mixer feed first — the decoder stalls while its buffers are full
      int frames, rate; uint64_t pts;
      int got = getPcmAac(player->aacDecoder, player->pcmScratch, &frames, &rate, &pts);
      if (got < 0) break;
      if (got == 1) {
         if (!player->audioClockValid) {   // anchor the A/V clock to the first audible frame
            // the feed and the A/V clock both assume the demuxer's rate; a decoder that disagrees
            // (e.g. SBR doubling the mp4a rate) means wrong pitch AND a drifting clock - make it visible
            int expectedRate = getAudioSource(player)->audioRate;
            if (rate != expectedRate) logWarn("[video-player] decoded audio rate %d Hz != demuxer rate %d Hz\n", rate, expectedRate);
            else logInfo("[video-player] audio up: %d Hz, %d samples/frame\n", rate, frames);
            player->audioClockBasePts = pts;
            __sync_synchronize();
            player->audioClockValid = 1;
         }
         int offset = 0;
         while (offset < frames && player->running) {
            offset += pushAudioPcm(player->pcmScratch + offset * 2, frames - offset);
            if (offset < frames) sleepMs(4);   // feed ring full: the mixer drains ~256 frames per block
         }
         continue;
      }

      // wrap the next demuxed AAC frame in an ADTS header when a scratch buffer is free
      if (!hasPendingFrame) {
         if (getAuBacklogAac(player->aacDecoder) > 1) { sleepMs(1); continue; }
         VideoDemuxer *source = getAudioSource(player);
         AudioAu au;
         // split streams read AAC frames directly (0 = end); a muxed track pops the queue the video
         // thread fills (0 = momentarily empty, more to come)
         int got = player->hasAudioDemuxer ? readAudioAu(source, &au) : takeAudioAu(source, &au);
         if (got <= 0) {
            if (player->hasAudioDemuxer) break;   // split: end of the audio stream
            sleepMs(4); continue;                 // muxed: queue empty for now
         }
         uint8_t *buffer = player->adtsBuffers[adtsIndex];
         if (buildAdtsHeader(buffer, au.size, source->audioAdtsRate, source->audioChannels) != 0) continue;
         memcpy(buffer + ADTS_HEADER_BYTES, au.data, au.size);
         pendingFrame = buffer;
         pendingSize  = au.size + ADTS_HEADER_BYTES;
         pendingPts   = au.pts;
         adtsIndex ^= 1;
         hasPendingFrame = 1;
      }

      int fed = decodeAuAac(player->aacDecoder, pendingFrame, pendingSize, pendingPts);
      if (fed < 0) break;
      if (fed == 0) { hasPendingFrame = 0; continue; }
      sleepMs(1);   // decoder full: loop back to pull PCM
   }
   player->audioAlive = 0;   // video pacing falls back to the wall clock
   exitThread();
}

// tears down whatever part of the audio pipeline came up (also the failure path during create)
static void releaseAudioPipeline(VideoPlayer *player)
{
   if (player->audioThreadActive) { joinThread(player->audioThread); player->audioThreadActive = 0; }
   if (player->audioFeedOpen) { closeAudioPcmFeed(); player->audioFeedOpen = 0; }
   destroyAacDecoder(player->aacDecoder);
   player->aacDecoder = 0;
   for (int i = 0; i < 2; i++) { free(player->adtsBuffers[i]); player->adtsBuffers[i] = 0; }
   free(player->pcmScratch);
   player->pcmScratch = 0;
}

// brings up the AAC decoder + mixer feed + audio thread; on any failure the video plays silent.
static void startAudioPipeline(VideoPlayer *player)
{
   VideoDemuxer *source = getAudioSource(player);
   if (!source->hasAudio) return;

   player->aacDecoder = createAacDecoder();
   player->adtsBuffers[0] = (uint8_t *)malloc(ADTS_HEADER_BYTES + AUDIO_AU_MAX_BYTES);
   player->adtsBuffers[1] = (uint8_t *)malloc(ADTS_HEADER_BYTES + AUDIO_AU_MAX_BYTES);
   player->pcmScratch = (float *)malloc(AAC_MAX_FRAME_SAMPLES * 2 * sizeof(float));
   player->audioFeedOpen = openAudioPcmFeed(source->audioRate) == 0;

   if (!player->aacDecoder || !player->adtsBuffers[0] || !player->adtsBuffers[1] || !player->pcmScratch || !player->audioFeedOpen) goto silent;

   // mark alive before the thread starts: the resume seek is posted right after open, and performSeek
   // must park (not race) the audio thread's first demuxer read. the thread clears this on exit.
   player->audioAlive = 1;
   player->audioThreadActive = (spawnJoinableThread(&player->audioThread, audioDecodeThread, (uint64_t)(uintptr_t)player,
                                THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "video-audio") == 0);
   if (!player->audioThreadActive) goto silent;
   return;

silent:
   player->audioAlive = 0;
   logWarn("[video-player] audio pipeline failed to start, playing silent\n");
   releaseAudioPipeline(player);
}

// ============================================================================
// lifecycle
// ============================================================================

static int roundUp16(int value) { return (value + 15) & ~15; }

// heap fallbacks when the caller supplies no frame allocator
static void *allocFrameFromHeap(size_t size) { return memalign(128, size); }
static void freeFrameToHeap(void *buffer) { free(buffer); }

VideoPlayer *createVideoPlayer(const char *path, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame)
{
   return createVideoPlayerSplit(path, 0, allocFrame, freeFrame);
}

VideoPlayer *createVideoPlayerSplit(const char *videoPath, const char *audioPath, VideoFrameAllocFn allocFrame, VideoFrameFreeFn freeFrame)
{
   VideoPlayer *player = (VideoPlayer *)calloc(1, sizeof *player);
   if (!player) return 0;
   player->displayIndex = -1;
   player->seekRequestNs = -1;
   player->audioSeekPendingNs = -1;
   player->freeFrame = freeFrame ? freeFrame : freeFrameToHeap;
   if (!allocFrame) allocFrame = allocFrameFromHeap;
   createLock(&player->lock);

   if (openVideoDemuxer(&player->demuxer, videoPath) != 0) goto fail;

   // adaptive streams carry audio in a separate url; open it as the audio source. failure just
   // means silent playback, so it never fails the whole player.
   if (audioPath && audioPath[0] && openAudioDemuxer(&player->audioDemuxer, audioPath) == 0)
      player->hasAudioDemuxer = 1;
   else if (audioPath && audioPath[0])
      logWarn("[video-player] separate audio stream failed to open, playing silent\n");

   // the decoder must be built for the stream's CODED size, which the SPS alone knows: the container
   // reports the DISPLAY size, and encoders pad beyond it (Intel QuickSync codes 720 as 736 and crops
   // it back). Get it wrong and cellVdec either decodes nothing at all or locks the console, so read
   // it from the SPS the demuxer already extracted, and only guess if there isn't one.
   H264StreamInfo streamInfo;
   if (readH264StreamInfo(player->demuxer.h264.header, player->demuxer.h264.headerSize, &streamInfo) == 0) {
      player->allocW = streamInfo.codedWidth;
      player->allocH = streamInfo.codedHeight;
   } else {
      player->allocW = roundUp16(player->demuxer.width);
      player->allocH = roundUp16(player->demuxer.height);
   }
   player->decoder = createH264Decoder(player->allocW, player->allocH, player->demuxer.level, player->demuxer.h264.maxRefFrames);
   if (!player->decoder) goto fail;

   // 1080p decode tops out around 35fps on the SPUs; faster streams (e.g. 1080p50 broadcasts) play
   // best-effort at that rate. (B_SKIP was tried and dropped: these encodes are ~87% disposable
   // B-frames, so skipping them leaves a ~7fps slideshow.)
   if (player->demuxer.frameDurationNs && player->demuxer.frameDurationNs < 32000000ull)
      logWarn("[video-player] %d fps exceeds decoder throughput, playback will drop frames\n",
              (int)(1000000000ull / player->demuxer.frameDurationNs));

   // YUV 4:2:0 planar frames; cellVdecGetPicture requires 128-byte alignment, size a multiple of 128
   size_t frameBytes = ((size_t)player->allocW * player->allocH * 3 / 2 + 127) & ~(size_t)127;
   for (int i = 0; i < FRAME_SLOTS; i++) {
      player->slots[i].yuv = (uint8_t *)allocFrame(frameBytes);
      if (!player->slots[i].yuv) goto fail;
   }

   player->running = 1;
   player->threadActive = (spawnJoinableThread(&player->thread, decodeThread, (uint64_t)(uintptr_t)player,
                           THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "video-player") == 0);
   if (!player->threadActive) goto fail;

   startAudioPipeline(player);
   // hold audio silent until the video pre-roll releases: otherwise audio drains during the ~0.7s wait
   // and its clock runs ahead, so video skips to catch up at the start (the "fast start"). getVideoFrame
   // resumes the feed at the first present, so audio and video begin together. Audio buffered meanwhile
   // just fills the feed ring, giving a head start on resume.
   if (player->audioFeedOpen) pauseAudioPcmFeed(1);
   return player;

fail:
   destroyVideoPlayer(player);
   return 0;
}

void destroyVideoPlayer(VideoPlayer *player)
{
   if (!player) return;
   player->running = 0;
   cancelVideoDemuxer(&player->demuxer);                                  // unblock a live read waiting on the edge
   if (player->hasAudioDemuxer) cancelVideoDemuxer(&player->audioDemuxer);
   if (player->threadActive) joinThread(player->thread);
   releaseAudioPipeline(player);
   destroyH264Decoder(player->decoder);
   closeVideoDemuxer(&player->demuxer);
   if (player->hasAudioDemuxer) closeVideoDemuxer(&player->audioDemuxer);
   for (int i = 0; i < FRAME_SLOTS; i++) if (player->slots[i].yuv) player->freeFrame(player->slots[i].yuv);
   destroyLock(&player->lock);
   free(player);
}

// ============================================================================
// presentation
// ============================================================================

const uint8_t *getVideoFrame(VideoPlayer *player, int *width, int *height)
{
   uint64_t nowUs = sys_time_get_system_time();
   int releaseAudio = 0;   // set when pre-roll fires, to un-hold the audio feed after the lock is dropped
   lock(&player->lock);

   // a posted seek freezes presentation: frames decoded before the flush must not advance the
   // position; the displayed frame stays up until the new position's frames arrive
   if (player->seekRequestNs >= 0) {
      int held = player->displayIndex;
      const uint8_t *heldBuffer = 0;
      if (held >= 0) { heldBuffer = player->slots[held].yuv; *width = player->slots[held].width; *height = player->slots[held].height; }
      unlock(&player->lock);
      return heldBuffer;
   }

   // anchor the clock to the earliest ready frame, with a short pre-roll: hold the first present until a
   // few frames are buffered (PREROLL_FRAMES) and the audio clock is live. Starting on the very first
   // decoded frame began playback with drops (the clock outran the cold decoder) and a visible stutter
   // when the audio clock later took over from the wall clock with a backward step.
   if (!player->started) {
      uint64_t earliest = 0; int found = -1, readyCount = 0;
      for (int i = 0; i < FRAME_SLOTS; i++)
         if (player->slots[i].state == SLOT_READY) { readyCount++; if (found < 0 || player->slots[i].pts < earliest) { earliest = player->slots[i].pts; found = i; } }
      int decoderWarm  = readyCount >= PREROLL_FRAMES || player->ended;      // a short clip may never fill the ring
      int audioWaiting = player->audioAlive && !player->audioClockValid;     // a dead/absent audio pipeline never holds video

      // don't wait forever for audio to anchor the clock: on a stalled audio stream it never decodes a
      // frame, audioClockValid never sets, and video would hang on a black screen (a "lockup"). After a
      // few seconds, start video without it rather than never starting.
      if (audioWaiting) {
         if (!player->audioWaitStartUs) player->audioWaitStartUs = nowUs;
         else if (nowUs - player->audioWaitStartUs > PREROLL_AUDIO_TIMEOUT_US) {
            logWarn("[video-player] audio clock never anchored, starting video without it\n");
            audioWaiting = 0;
         }
      }
      if (found < 0 || !decoderWarm || audioWaiting) { unlock(&player->lock); return 0; }
      player->basePts    = earliest;
      player->baseWallUs = nowUs;
      player->started    = 1;
      releaseAudio       = 1;   // start the audio feed together with the first video frame
   }

   // the audio clock is the sync master while audio runs: it only advances as samples reach the
   // speakers, so video locked to it cannot drift. When audio ends or dies, re-anchor the wall clock
   // at the current position and pace on that instead.
   if (player->audioClockValid && (!player->audioAlive || player->ended)) {
      player->audioClockValid = 0;
      player->basePts    = player->currentPts;
      player->baseWallUs = nowUs;
   }

   uint64_t clock;
   if (player->audioClockValid && player->started)
      clock = player->audioClockBasePts + getAudioPcmFeedPlayedFrames() * 1000000000ull / (uint64_t)getAudioSource(player)->audioRate;
   else if (player->paused)
      clock = player->currentPts;
   else
      clock = player->basePts + (nowUs - player->baseWallUs) * 1000;   // us -> ns

   // pick the latest ready frame whose pts is due; drop older ready frames (skip if we fell behind)
   int best = -1; uint64_t bestPts = 0;
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_READY && player->slots[i].pts <= clock && (best < 0 || player->slots[i].pts >= bestPts)) { best = i; bestPts = player->slots[i].pts; }

   if (best >= 0) {
      for (int i = 0; i < FRAME_SLOTS; i++) {
         if (player->slots[i].state == SLOT_RETIRING) player->slots[i].state = SLOT_EMPTY;                // GPU is done with it by now
         else if (player->slots[i].state == SLOT_DISPLAYING) player->slots[i].state = SLOT_RETIRING;      // hold one present for the frame in flight
         else if (player->slots[i].state == SLOT_READY && player->slots[i].pts < bestPts) player->slots[i].state = SLOT_EMPTY;   // drop skipped
      }
      player->slots[best].state = SLOT_DISPLAYING;
      player->displayIndex = best;
      player->currentPts   = bestPts;
   }

   int idx = player->displayIndex;
   const uint8_t *buffer = 0;
   if (idx >= 0) { buffer = player->slots[idx].yuv; *width = player->slots[idx].width; *height = player->slots[idx].height; }
   unlock(&player->lock);

   // release the audio held through pre-roll (mixer call must run outside player->lock)
   if (releaseAudio && player->audioFeedOpen) pauseAudioPcmFeed(0);
   return buffer;
}

void setVideoPaused(VideoPlayer *player, int paused)
{
   uint64_t nowUs = sys_time_get_system_time();
   lock(&player->lock);
   if (paused && !player->paused) {
      player->paused = 1;
      player->pauseWallUs = nowUs;
   } else if (!paused && player->paused) {
      player->paused = 0;
      player->baseWallUs += nowUs - player->pauseWallUs;   // don't count paused time against the clock
   }
   unlock(&player->lock);
   if (player->audioFeedOpen) pauseAudioPcmFeed(paused);
}

int isVideoPaused(const VideoPlayer *player) { return player->paused; }

// post a seek target for the decode thread. landAfter=1 rounds up to the next keyframe at/after `seconds`
// (used to skip fully past a sponsor segment); landAfter=0 lands on the keyframe covering it (normal scrub).
static void postSeek(VideoPlayer *player, float seconds, int landAfter)
{
   if (seconds < 0.0f) seconds = 0.0f;
   float duration = getVideoDurationSeconds(player);
   if (duration > 0.0f && seconds > duration) seconds = duration;
   int64_t targetNs = (int64_t)(seconds * 1.0e9f);
   lock(&player->lock);
   player->currentPts = (uint64_t)targetNs;   // position reads as the target until the seek lands
   unlock(&player->lock);
   player->seekLandAfter = landAfter;
   __sync_synchronize();                       // publish the land direction before the decode thread sees the request
   player->seekRequestNs = targetNs;
}

void seekVideoPlayer(VideoPlayer *player, float seconds)     { postSeek(player, seconds, 0); }
void seekVideoPlayerPast(VideoPlayer *player, float seconds) { postSeek(player, seconds, 1); }

int isVideoEnded(const VideoPlayer *player)
{
   if (!player->ended) return 0;
   for (int i = 0; i < FRAME_SLOTS; i++)
      if (player->slots[i].state == SLOT_READY || player->slots[i].state == SLOT_FILLING) return 0;
   return 1;
}

float getVideoPositionSeconds(const VideoPlayer *player)
{
   return (float)player->currentPts / 1.0e9f;   // stream pts count from the start of the file
}

float getVideoDurationSeconds(const VideoPlayer *player)
{
   return (float)player->demuxer.durationNs / 1.0e9f;
}

// audio track format for a stats overlay; rate/channels read 0 when there's no audio track.
void getAudioTrackInfo(const VideoPlayer *player, int *rate, int *channels)
{
   const VideoDemuxer *source = player->hasAudioDemuxer ? &player->audioDemuxer : &player->demuxer;
   if (rate)     *rate = source->audioRate;
   if (channels) *channels = source->audioChannels;
}
