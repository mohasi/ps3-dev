// play screen - resolve a video, then stream it with the simple-lib-av engine.
//
// Both stream urls are handed straight to the player: simple-lib-av opens each through
// openHttpStream (the http module), so each demuxer reads the moov and then each sample
// on demand by HTTP range - nothing is downloaded in full. Adaptive picks are split: a
// video-only stream plus an audio-only stream, each its own independent http stream.
//
// Follows file-manager's video overlay: the open work runs on a worker
// (createVideoPlayer does blocking network I/O and must stay off the UI thread),
// the UI thread adopts the built player and pulls frames each render frame.

#include "screens/play.h"
#include "extractor.h"
#include "stream-select.h"   // pickBestVideo / pickBestAudio (shared with download)

#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "button-repeat.h"     // left/right scrub auto-repeat
#include "thread.h"
#include "screen-manager.h"
#include "string-utilities.h"   // strCopy

#include "video-player.h"
#include "audio.h"              // setAudioPcmFeedVolume
#include "storage.h"            // resume position (get/setWatchedPosition)
#include "sponsorblock.h"       // skip segments (auto-skip + seek-bar marks)
#include "dbg.h"                // logInfo/logError (bridge)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>              // snprintf (stats overlay)
#include <sys/sys_time.h>       // sys_time_get_system_time (fps measure)

#define PLAY_VOLUME 0.8f

// seek controls (bar + time) show on activity / pause, then auto-hide. left/right scrub the target
// instantly; the engine seek (a decoder flush + fragment reload each time) fires once input goes quiet.
#define SEEK_STEP_SECONDS   10.0f
#define SEEK_APPLY_IDLE_US  400000ULL
#define CONTROLS_VISIBLE_US 3000000ULL
#define SKIP_NOTICE_US      2000000ULL   // how long the "Skipped ..." toast stays up

// the video to play, set by playVideo() before the screen is pushed. defaults to
// a short public clip so pushing the screen directly still does something.
static char requestedInput[256] = "jNQXAC9IVRw";   // "Me at the zoo"

typedef enum { STAGE_LOADING, STAGE_FAILED, STAGE_PLAYING } Stage;

static struct {
   volatile Stage   stage;
   char             message[128];     // failure reason (worker writes, UI reads)
   VideoPlayer     *pendingPlayer;    // built by the worker, adopted by the UI thread
   volatile int     workerDone;
   int              threadActive;
   sys_ppu_thread_t workerTid;
   VideoPlayer     *player;
   int              screenW, screenH;
   int              firstFrameSeen;   // a real frame has been presented (loading status ends, save is safe)
   int              showStats;        // SELECT toggles a debug overlay
   int              vidItag, vidW, vidH, vidFps, audItag;   // picked formats (worker sets, UI reads)
   const uint8_t   *lastFrame;        // fps measure: a changed pointer means a newly presented frame
   int              framesThisSecond, measuredFps;
   uint64_t         fpsTickUs;

   // seek: the UI owns the scrub target so the bar moves instantly; the engine seek is deferred until
   // the input goes quiet (each apply costs a decoder flush + fragment reload)
   int              seeking;
   float            seekTarget;
   uint64_t         lastSeekInputUs;
   uint64_t         controlsShownUs;   // last activity, for the bar/time auto-hide

   // sponsorblock: fetched on a side thread (in parallel with resolve/open), then auto-skipped during
   // playback and drawn on the seek bar. sbReady goes up once the thread is reaped and segments are safe.
   SponsorSegments  sb;
   volatile int     sbWorkerDone;
   int              sbThreadActive, sbReady;
   sys_ppu_thread_t sbWorkerTid;
   uint64_t         skipNoticeUs;      // brief "Skipped ..." toast timer (0 = hidden)
} state;

#define STAT_LINES 4

static Font  font;
static Label statusLabel;
static Label statLabels[STAT_LINES];
static Label timeLeftLabel, timeRightLabel;   // seek bar: current time (left) / total (right)
static Label skipNoticeLabel;                 // brief "Skipped ..." toast after an auto-skip
static uint64_t playRequestUs;   // TEMP: set when the user picks a video, to time the WHOLE select->picture path

static void fail(const char *reason)
{
   logError("[yt] play failed: %s\n", reason);
   strCopy(state.message, sizeof state.message, reason);
   __sync_synchronize();   // publish message before the UI thread sees STAGE_FAILED
   state.stage = STAGE_FAILED;
}

static void worker(uint64_t arg)
{
   (void)arg;
   uint64_t tPlayStart = sys_time_get_system_time();   // TEMP: full resume-path timing (resolve -> open -> seek)
   logInfo("[yt] diag worker started %llums after select\n", (unsigned long long)((tPlayStart - playRequestUs) / 1000));   // TEMP
   StreamInfo *info = malloc(sizeof *info);   // ~50 KB, too big for the stack
   const Extractor *extractor = findExtractor(requestedInput);
   if (!info || !extractor) { fail("unrecognised link or id"); goto done; }

   // resolve (stage stays STAGE_LOADING, set by initPlay's memset, until we adopt or fail)
   if (extractor->extract(requestedInput, info) != 0 || info->formatCount == 0) { fail("resolve failed"); goto done; }
   logInfo("[yt] diag resolve took %llums\n", (unsigned long long)((sys_time_get_system_time() - tPlayStart) / 1000));   // TEMP

   const StreamFormat *video = pickBestVideo(info);
   if (!video) { fail("no playable mp4 video"); goto done; }
   // a muxed pick (itag 18) already carries audio; a video-only pick needs a separate audio track
   const StreamFormat *audio = video->hasAudio ? NULL : pickBestAudio(info);
   logInfo("[yt] play video itag %d %dx%d %s, audio itag %d\n", video->itag, video->width, video->height,
           video->hasAudio ? "muxed" : "video-only", audio ? audio->itag : (video->hasAudio ? video->itag : 0));

   state.vidItag = video->itag; state.vidW = video->width; state.vidH = video->height; state.vidFps = video->fps;
   state.audItag = audio ? audio->itag : (video->hasAudio ? video->itag : 0);

   // open the decoder: both streams ride the http module by range request. an audio open
   // failure inside the player just means silent playback. NULL if it can't be demuxed/decoded.
   uint64_t tBeforeOpen = sys_time_get_system_time();   // TEMP
   state.pendingPlayer = createVideoPlayerSplit(video->url, audio ? audio->url : NULL, allocGfxVideoBuffer, freeGfxVideoBuffer);
   if (!state.pendingPlayer) { fail("couldn't open stream"); goto done; }
   logInfo("[yt] diag open both streams took %llums (total since play %llums)\n",   // TEMP
           (unsigned long long)((sys_time_get_system_time() - tBeforeOpen) / 1000),
           (unsigned long long)((sys_time_get_system_time() - tPlayStart) / 1000));

   // resume where we left off. seekVideoPlayer only posts the target; the decode thread does the
   // reconnect + fragment reload. drawPlay keeps the "Loading..." status up until the first frame is
   // presented, so that reload is covered rather than showing a black screen. storage returns 0 for a
   // typed url (never a history key), so this no-ops for anything but a bare videoId.
   {
      int resumeAt = getWatchedPosition(requestedInput);
      float duration = getVideoDurationSeconds(state.pendingPlayer);
      if (resumeAt > 3 && (duration <= 0 || resumeAt < duration - 10)) seekVideoPlayer(state.pendingPlayer, (float)resumeAt);
   }

done:
   free(info);
   __sync_synchronize();
   state.workerDone = 1;
   exitThread();
}

// a bare 11-char videoId (not a typed url) is what SponsorBlock and history key on; typed urls no-op.
static int isBareVideoId(const char *input)
{
   return input[0] && !strchr(input, '/') && !strchr(input, ':');
}

// fetch skip segments in parallel with resolve/open, so they cost no startup latency.
static void sponsorWorker(uint64_t arg)
{
   (void)arg;
   fetchSponsorSegments(requestedInput, &state.sb);
   __sync_synchronize();
   state.sbWorkerDone = 1;
   exitThread();
}

void playVideo(const char *input)
{
   strCopy(requestedInput, sizeof requestedInput, input);
   playRequestUs = sys_time_get_system_time();   // TEMP: start of the true perceived resume
   pushScreen(&playScreen);
}

static void initPlay(void)
{
   memset(&state, 0, sizeof state);
   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();

   font = openSystemFont(FONT_POP);
   initLabel(&statusLabel, &font, 0, 0, 1400, AUTO, 28, COLOR_SLATE_100, TEXT_NOWRAP, "");
   for (int i = 0; i < STAT_LINES; i++)
      initLabel(&statLabels[i], &font, 0, 0, 600, AUTO, 22, COLOR_SLATE_100, TEXT_NOWRAP, "");
   initLabel(&timeLeftLabel,  &font, 0, 0, 200, AUTO, 22, COLOR_SLATE_100, TEXT_NOWRAP, "");
   initLabel(&timeRightLabel, &font, 0, 0, 200, AUTO, 22, COLOR_SLATE_100, TEXT_NOWRAP, "");
   initLabel(&skipNoticeLabel, &font, 0, 0, 400, AUTO, 22, COLOR_SLATE_100, TEXT_NOWRAP, "");

   state.threadActive = (spawnJoinableThread(&state.workerTid, worker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-play") == 0);
   if (!state.threadActive) fail("couldn't start worker");

   if (isBareVideoId(requestedInput))
      state.sbThreadActive = (spawnJoinableThread(&state.sbWorkerTid, sponsorWorker, 0,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-sponsor") == 0);
}

static void showControls(void) { state.controlsShownUs = sys_time_get_system_time(); }

// accumulates a scrub target while left/right is held; the engine seek fires once input goes quiet
static void nudgeSeek(float deltaSeconds, uint64_t nowUs)
{
   if (!state.seeking) { state.seeking = 1; state.seekTarget = getVideoPositionSeconds(state.player); }
   state.seekTarget += deltaSeconds;
   float duration = getVideoDurationSeconds(state.player);
   if (state.seekTarget < 0.0f) state.seekTarget = 0.0f;
   if (duration > 0.0f && state.seekTarget > duration) state.seekTarget = duration;
   state.lastSeekInputUs = nowUs;
   showControls();
}

// left/right scrub, cross toggles pause (restarts if the video ended). only once playing.
static void handlePlaybackInput(void)
{
   uint64_t nowUs = sys_time_get_system_time();

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (isVideoEnded(state.player)) seekVideoPlayer(state.player, 0.0f);
      else setVideoPaused(state.player, !isVideoPaused(state.player));
      showControls();
   }

   static ButtonRepeat seekRepeat;
   if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_RIGHT)))     nudgeSeek(+SEEK_STEP_SECONDS, nowUs);
   else if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_LEFT))) nudgeSeek(-SEEK_STEP_SECONDS, nowUs);

   if (state.seeking && nowUs - state.lastSeekInputUs >= SEEK_APPLY_IDLE_US) {
      seekVideoPlayer(state.player, state.seekTarget);
      state.seeking = 0;
   }
}

// jump past the first not-yet-skipped sponsor segment the playhead has entered. one-shot per segment so a
// manual rewind into it doesn't fight the user.
static void autoSkipSponsor(void)
{
   float pos = getVideoPositionSeconds(state.player);
   for (int i = 0; i < state.sb.count; i++) {
      SponsorSegment *segment = &state.sb.segments[i];
      if (segment->skipped || pos < segment->start || pos >= segment->end - 0.2f) continue;
      segment->skipped = 1;
      seekVideoPlayer(state.player, segment->end);
      char notice[64];
      snprintf(notice, sizeof notice, "Skipped %s", getSponsorCategoryName(segment->category));
      setLabelText(&skipNoticeLabel, notice);
      state.skipNoticeUs = sys_time_get_system_time();
      showControls();
      return;
   }
}

static void updatePlay(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { popScreen(); return; }   // back to the results list
   if (isPadButtonPressed(PAD_BTN_SELECT)) state.showStats = !state.showStats;

   // reap the worker once it's done; adopt the player it built
   if (state.workerDone && state.threadActive) {
      joinThread(state.workerTid);
      state.threadActive = 0;
      if (state.pendingPlayer) {
         state.player = state.pendingPlayer;
         state.pendingPlayer = NULL;
         setAudioPcmFeedVolume(PLAY_VOLUME);
         state.stage = STAGE_PLAYING;
      }
   }

   // reap the sponsorblock fetch; its segments are safe to read once joined
   if (state.sbWorkerDone && state.sbThreadActive) {
      joinThread(state.sbWorkerTid);
      state.sbThreadActive = 0;
      state.sbReady = 1;
   }

   if (state.stage == STAGE_PLAYING && state.player) handlePlaybackInput();
   if (state.sbReady && state.stage == STAGE_PLAYING && state.player && !isVideoPaused(state.player) && !state.seeking)
      autoSkipSponsor();

   // status text: the failure reason, or "Loading..." until playback. setLabelText skips unchanged
   // text, so calling it each frame only rasterises on a real change.
   if (state.stage == STAGE_FAILED) setLabelText(&statusLabel, state.message);
   else if (state.stage != STAGE_PLAYING) setLabelText(&statusLabel, "Loading...");
}

// fits w x h inside the screen preserving aspect ratio; returns the centred rect.
static void letterboxRect(int w, int h, int *dx, int *dy, int *dw, int *dh)
{
   float frameAspect  = (float)w / (float)h;
   float screenAspect = (float)state.screenW / (float)state.screenH;
   if (frameAspect > screenAspect) { *dw = state.screenW; *dh = (int)(state.screenW / frameAspect); }
   else                            { *dh = state.screenH; *dw = (int)(state.screenH * frameAspect); }
   *dx = (state.screenW - *dw) / 2;
   *dy = (state.screenH - *dh) / 2;
}

// measure the real presented frame rate: a changed frame pointer is a new frame; tally them per second.
static void measureFps(const uint8_t *frame)
{
   if (frame && frame != state.lastFrame) { state.framesThisSecond++; state.lastFrame = frame; }
   uint64_t now = sys_time_get_system_time();
   if (now - state.fpsTickUs >= 1000000) { state.measuredFps = state.framesThisSecond; state.framesThisSecond = 0; state.fpsTickUs = now; }
}

// rebuild the overlay lines. setLabelText skips unchanged text, so calling this per frame only
// rasterises when a value actually moves (fps + time once a second).
static void updateStatLabels(void)
{
   char line[96];
   int rate = 0, channels = 0;
   getAudioTrackInfo(state.player, &rate, &channels);
   float pos = getVideoPositionSeconds(state.player), duration = getVideoDurationSeconds(state.player);

   snprintf(line, sizeof line, "Video  itag %d   %dx%d   H.264", state.vidItag, state.vidW, state.vidH);
   setLabelText(&statLabels[0], line);
   snprintf(line, sizeof line, "FPS  %d   (target %d)", state.measuredFps, state.vidFps);
   setLabelText(&statLabels[1], line);
   if (state.audItag) snprintf(line, sizeof line, "Audio  itag %d   AAC %d Hz   %d ch", state.audItag, rate, channels);
   else               strCopy(line, sizeof line, "Audio  none");
   setLabelText(&statLabels[2], line);
   snprintf(line, sizeof line, "Time  %d:%02d / %d:%02d", (int)pos / 60, (int)pos % 60, (int)duration / 60, (int)duration % 60);
   setLabelText(&statLabels[3], line);
}

static void drawStatsOverlay(void)
{
   int x = 40, y = 44, lineHeight = 30;
   fillGfxRectangle(x - 14, y - 12, 500, STAT_LINES * lineHeight + 14, 0xC0000000);
   for (int i = 0; i < STAT_LINES; i++) drawLabelAt(&statLabels[i], x, y + i * lineHeight);
}

// seek bar + time show while scrubbing / paused / ended, else auto-hide after the last activity
static int controlsVisible(void)
{
   if (isVideoPaused(state.player) || isVideoEnded(state.player) || state.seeking) return 1;
   return state.controlsShownUs != 0 && sys_time_get_system_time() - state.controlsShownUs < CONTROLS_VISIBLE_US;
}

static void formatTime(char *buffer, int cap, int seconds)
{
   if (seconds < 0) seconds = 0;
   if (seconds >= 3600) snprintf(buffer, cap, "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
   else                 snprintf(buffer, cap, "%d:%02d", seconds / 60, seconds % 60);
}

// paint each sponsor segment onto the bar in its category colour, over the progress fill so it stays
// visible in the already-watched region (as YouTube's SponsorBlock overlay does).
static void drawSponsorSegments(int barLeft, int span, int barTopY, int barH, float duration)
{
   if (duration <= 0.0f) return;
   for (int i = 0; i < state.sb.count; i++) {
      const SponsorSegment *segment = &state.sb.segments[i];
      float startFrac = segment->start / duration, endFrac = segment->end / duration;
      if (startFrac < 0.0f) startFrac = 0.0f;
      if (endFrac > 1.0f) endFrac = 1.0f;
      int segX = barLeft + (int)(startFrac * span);
      int segW = (int)((endFrac - startFrac) * span);
      if (segW < 2) segW = 2;
      fillGfxRectangle(segX, barTopY, segW, barH, getSponsorCategoryColor(segment->category));
   }
}

// a red YouTube-style progress bar low on the screen with current/total time on each side, drawn from
// plain rectangles (no spritesheet), with sponsor segments marked over the fill.
static void drawSeekBar(void)
{
   float duration = getVideoDurationSeconds(state.player);
   float position = state.seeking ? state.seekTarget : getVideoPositionSeconds(state.player);
   float progress = duration > 0.0f ? position / duration : 0.0f;
   if (progress < 0.0f) progress = 0.0f;
   if (progress > 1.0f) progress = 1.0f;

   int barLeft  = (int)(state.screenW * 0.24f);
   int barRight = state.screenW - barLeft;
   int span     = barRight - barLeft;
   int barY     = (int)(state.screenH * 0.94f);
   int barH     = 8;
   int barTopY  = barY - barH / 2;
   int filledW  = (int)(progress * span);

   fillGfxRectangle(barLeft, barTopY, span, barH, 0x66FFFFFF);            // track
   fillGfxRectangle(barLeft, barTopY, filledW, barH, 0xFFFF0000);        // progress (youtube red)
   if (state.sbReady) drawSponsorSegments(barLeft, span, barTopY, barH, duration);
   fillGfxRectangle(barLeft + filledW - 3, barY - 11, 6, 22, 0xFFFFFFFF);   // scrubber handle

   char text[16];
   formatTime(text, sizeof text, (int)position);            setLabelText(&timeLeftLabel, text);
   formatTime(text, sizeof text, (int)(duration + 0.5f));   setLabelText(&timeRightLabel, text);
   drawLabelAt(&timeLeftLabel,  barLeft - 20 - timeLeftLabel.tt.tex.w, barY - timeLeftLabel.tt.tex.h / 2);
   drawLabelAt(&timeRightLabel, barRight + 20, barY - timeRightLabel.tt.tex.h / 2);
}

// the "Skipped ..." toast, centred just above the seek bar for a moment after an auto-skip.
static void drawSkipNotice(void)
{
   if (state.skipNoticeUs == 0 || sys_time_get_system_time() - state.skipNoticeUs >= SKIP_NOTICE_US) return;
   int barY = (int)(state.screenH * 0.94f);
   int x = state.screenW / 2 - skipNoticeLabel.tt.tex.w / 2;
   int y = barY - 60;
   fillGfxRectangle(x - 14, y - 6, skipNoticeLabel.tt.tex.w + 28, skipNoticeLabel.tt.tex.h + 12, 0xC0000000);
   drawLabelAt(&skipNoticeLabel, x, y);
}

static void drawPlay(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, COLOR_BLACK);   // letterbox bars read as black, not slate

   const uint8_t *frame = NULL;
   if (state.player) {
      int w, h;
      frame = getVideoFrame(state.player, &w, &h);
      if (frame) {
         if (!state.firstFrameSeen)   // TEMP: the whole thing the user actually waits for
            logInfo("[yt] diag select-to-picture %llums\n", (unsigned long long)((sys_time_get_system_time() - playRequestUs) / 1000));
         state.firstFrameSeen = 1;
         int dx, dy, dw, dh;
         letterboxRect(w, h, &dx, &dy, &dw, &dh);
         drawGfxYuvFrame(dx, dy, dw, dh, frame, w, h);
      }
      measureFps(frame);
      if (state.showStats && frame) { updateStatLabels(); drawStatsOverlay(); }
      if (state.stage == STAGE_PLAYING && controlsVisible()) drawSeekBar();
      if (state.stage == STAGE_PLAYING) drawSkipNotice();
   }

   // until the first frame is presented (open, then the async resume seek's reload) keep the loading
   // status up instead of a black screen; with no player this shows the failure message.
   if (!state.firstFrameSeen)
      drawLabelAt(&statusLabel, state.screenW / 2 - statusLabel.tt.tex.w / 2, state.screenH / 2 - 14);
}

static void termPlay(void)
{
   if (state.threadActive) { joinThread(state.workerTid); state.threadActive = 0; }
   if (state.sbThreadActive) { joinThread(state.sbWorkerTid); state.sbThreadActive = 0; }   // usually already done
   // save the resume position on the way out; a finished video is saved as 0 so it restarts next time.
   // only once a frame has actually played: a failed open or a resume seek that never landed must not
   // clobber a good saved position with 0. storage ignores non-id keys, so typed urls no-op.
   if (state.player && state.firstFrameSeen)
      setWatchedPosition(requestedInput, isVideoEnded(state.player) ? 0 : (int)getVideoPositionSeconds(state.player));
   if (state.player || state.pendingPlayer) finishGfx();   // RSX may still be sampling a frame buffer
   if (state.player) { destroyVideoPlayer(state.player); state.player = NULL; }
   if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = NULL; }
   freeLabel(&statusLabel);
   for (int i = 0; i < STAT_LINES; i++) freeLabel(&statLabels[i]);
   freeLabel(&timeLeftLabel);
   freeLabel(&timeRightLabel);
   freeLabel(&skipNoticeLabel);
   closeFont(&font);
}

Screen playScreen = { initPlay, NULL, updatePlay, drawPlay, NULL, termPlay, SCREEN_TERMINATED };
