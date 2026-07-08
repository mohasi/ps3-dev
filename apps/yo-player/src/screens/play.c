// play screen - resolve a video, then stream it with the simple-lib-av engine.
//
// Both stream urls are handed straight to the player: simple-lib-av opens them through
// the VFS, which routes http(s):// to the http-fs backend, so each demuxer reads
// the moov and then each sample on demand by HTTP range - nothing is downloaded
// in full. Adaptive picks are split: a video-only stream plus an audio-only stream,
// each on its own http-fs stream/client (http-fs serialises the shared libhttp pools).
// (Audio used to be pre-downloaded because a second live stream returned garbage; the
// actual culprit was httpFetch destroying truncated responses mid-body on a keep-alive
// client, fixed in http-fetch.c - so audio streams live again.)
//
// Follows file-manager's video overlay: the open work runs on a worker
// (createVideoPlayer does blocking network I/O and must stay off the UI thread),
// the UI thread adopts the built player and pulls frames each render frame.

#include "screens/play.h"
#include "extractor.h"

#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "thread.h"
#include "screen-manager.h"
#include "string-utilities.h"   // strCopy

#include "video-player.h"
#include "audio.h"              // setAudioPcmFeedVolume
#include "storage.h"            // resume position (get/setWatchedPosition)
#include "dbg.h"                // logInfo/logError (bridge)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>              // snprintf (stats overlay)
#include <sys/sys_time.h>       // sys_time_get_system_time (fps measure)

#define PLAY_VOLUME 0.8f

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
} state;

#define STAT_LINES 4

static Font  font;
static Label statusLabel;
static Label statLabels[STAT_LINES];
static uint64_t playRequestUs;   // TEMP: set when the user picks a video, to time the WHOLE select->picture path

static void fail(const char *reason)
{
   logError("[yt] play failed: %s\n", reason);
   strCopy(state.message, sizeof state.message, reason);
   __sync_synchronize();   // publish message before the UI thread sees STAGE_FAILED
   state.stage = STAGE_FAILED;
}

// what the PS3 H.264 decoder can keep up with: 1080p at <=30 fps, or 720p at <=60 fps (fewer pixels/sec
// than 1080p30, so it stays within throughput). 1080p60 (itag 298/299) exceeds it and is skipped - such
// a video falls to 720p60 rather than dropping all the way to a 30 fps 480p variant.
static int decodableVideo(const StreamFormat *f)
{
   if (f->height <= 1080 && f->fps <= 30) return 1;
   if (f->height <= 720  && f->fps <= 60) return 1;
   return 0;
}

// pick the best decodable H.264/mp4 stream: highest resolution, and at a given resolution the higher
// frame rate. Favours the adaptive (video-only) streams over the 360p muxed one; audio is a separate
// track (pickAudio).
static const StreamFormat *pickVideo(const StreamInfo *info)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;   // mp4 == avc1 for these itags
      if (!decodableVideo(format)) continue;
      if (!best || format->height > best->height || (format->height == best->height && format->fps > best->fps))
         best = format;
   }
   return best;
}

// pick the AAC/mp4 audio-only stream to pair with a video-only pick, preferring itag 140
// (128k AAC-LC). skips opus/webm, which the AAC decoder can't play.
static const StreamFormat *pickAudio(const StreamInfo *info)
{
   const StreamFormat *best = NULL;
   for (int i = 0; i < info->formatCount; i++) {
      const StreamFormat *format = &info->formats[i];
      if (!format->hasAudio || format->hasVideo || format->needsCipher || !format->url[0]) continue;
      if (strcmp(format->container, "mp4") != 0) continue;
      if (!best || format->itag == 140) best = format;
   }
   return best;
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

   const StreamFormat *video = pickVideo(info);
   if (!video) { fail("no playable mp4 video"); goto done; }
   // a muxed pick (itag 18) already carries audio; a video-only pick needs a separate audio track
   const StreamFormat *audio = video->hasAudio ? NULL : pickAudio(info);
   logInfo("[yt] play video itag %d %dx%d %s, audio itag %d\n", video->itag, video->width, video->height,
           video->hasAudio ? "muxed" : "video-only", audio ? audio->itag : (video->hasAudio ? video->itag : 0));

   state.vidItag = video->itag; state.vidW = video->width; state.vidH = video->height; state.vidFps = video->fps;
   state.audItag = audio ? audio->itag : (video->hasAudio ? video->itag : 0);

   // open the decoder: both streams ride http-fs by range, each on its own client. an audio open
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

   state.threadActive = (spawnJoinableThread(&state.workerTid, worker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-play") == 0);
   if (!state.threadActive) fail("couldn't start worker");
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
   }

   // until the first frame is presented (open, then the async resume seek's reload) keep the loading
   // status up instead of a black screen; with no player this shows the failure message.
   if (!state.firstFrameSeen)
      drawLabelAt(&statusLabel, state.screenW / 2 - statusLabel.tt.tex.w / 2, state.screenH / 2 - 14);
}

static void termPlay(void)
{
   if (state.threadActive) { joinThread(state.workerTid); state.threadActive = 0; }
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
   closeFont(&font);
}

Screen playScreen = { initPlay, NULL, updatePlay, drawPlay, NULL, termPlay, SCREEN_TERMINATED };
