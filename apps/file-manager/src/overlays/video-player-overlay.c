// video-player-overlay - full-screen video player. See video-player-overlay.h.
//
// A playable file streams through the VideoPlayer engine - decoded ahead on a worker, frames paced
// on the audio clock (wall clock when there's no audio), drawn letterboxed on a dark scrim.
// Controls: Cross play/pause (restarts after the end), left/right seek (scrub, applied on release),
// up/down volume, L1/R1 prev/next video in the folder, Circle closes. The seek bar + filename
// caption show on activity / pause / end and auto-hide; unsupported files show the probe's reason.
#include "overlays/video-player-overlay.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "colors.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/slice.h"
#include "thread.h"             // spawnJoinableThread, joinThread
#include "vfs.h"               // getBaseName, MAX_PATH_LEN
#include "string-utilities.h"   // strCopy
#include "sprite-regions.h"
#include "video-probe.h"
#include "video-player.h"
#include "audio.h"              // setAudioPcmFeedVolume: the video feed's volume
#include "button-repeat.h"
#include "dir-playlist.h"       // folder scan + prev/next-with-wrap navigation
#include "dbg.h"

#include <string.h>
#include <stdio.h>
#include <sys/sys_time.h>

#define COLOR_SCRIM     0xF2000000u   // near-black backdrop / letterbox bars
#define COLOR_MESSAGE   0xFFFFFFFFu
#define COLOR_STATUS    0x99FFFFFFu
#define COLOR_TIME_DIM  0x99FFFFFFu

#define MESSAGE_SIZE    30
#define TIME_SIDE_SIZE  20

// controls (seek bar + filename caption) show on input / pause / end, then auto-hide
#define CONTROLS_VISIBLE_US 3000000ULL
#define SEEK_STEP_SECONDS   10.0f
#define SEEK_APPLY_IDLE_US  400000ULL   // scrub target applies once the input goes quiet

// filename caption, top-left (mirrors the image viewer)
#define NAME_X          40
#define NAME_Y          28
#define NAME_SIZE       24
#define NAME_MAX_WIDTH  1400
#define COLOR_CAPTION_BG 0x80000000u
#define CAPTION_PAD_X   12
#define CAPTION_PAD_Y   6

// seek bar geometry (mirrors the audio player, sat lower on the screen)
#define BAR_H           10
#define BAR_CAP         5
#define THUMB_DIA       28
#define SIDE_TIME_GAP   24

// volume meter (mirrors the audio player: a pill column that auto-hides)
#define VOLUME_PILLS       15
#define VOLUME_DEFAULT     10
#define VOLUME_VISIBLE_US  2500000ULL
#define VOL_NUM_SIZE       22
#define VOL_PILL_W         31
#define VOL_PILL_H         10
#define VOL_PILL_PITCH     19
#define VOL_SPEAKER_W      32
#define VOL_SPEAKER_H      29
#define VOL_SPEAKER_GAP    22

static struct {
   int  screenW, screenH;
   int  centerX, centerY;
   char name[256];
   char path[MAX_PATH_LEN];

   int          working;
   int          threadActive;
   volatile int workerDone;
   VideoPlayability pending;
   VideoPlayer *pendingPlayer;   // built by the worker for a playable file
   sys_ppu_thread_t workerTid;

   VideoPlayability result;
   int          haveResult;

   VideoPlayer *player;          // frames draw zero-copy from RSX-mapped memory (drawGfxYuvFrame)

   uint64_t     volumeShownUs;   // last volume change, for the meter auto-hide
   uint64_t     controlsShownUs; // last user activity, for the seek bar / caption auto-hide

   // left/right scrubbing: the UI owns the target so the bar moves instantly; the engine seek is
   // applied once the input goes quiet (each apply costs a decoder flush)
   int          seeking;
   float        seekTarget;
   uint64_t     lastSeekInputUs;

   DirPlaylist  playlist;        // sibling videos in the folder, for L1/R1 navigation

   int          barLeft, barRight, barY;
   int          volPillX, volBottomY;   // left edge of pills; top-y of the lowest pill
} state;

// volume level (0..VOLUME_PILLS) kept outside `state` so it survives re-opens; shared feel with the
// audio player but tracked separately (it drives the video feed's volume, not a mixer stream's).
static int volumeLevel = VOLUME_DEFAULT;

static GfxTexture sprites;

static Font  font;
static int   ready;
static Label messageLabel, statusLabel, nameLabel, timeLeftLabel, timeRightLabel, volNumLabel;
static Image thumbImg, speakerImg, pillBlue, pillGrey;
static Slice trackSlice, fillSlice;
static int   lastElapsed = -1, lastRemain = -1, lastVolNum = -1;

static void show(void);
static void hide(void);
static void update(void);
static void draw(void);
static void term(void);
static void worker(uint64_t arg);

Overlay videoPlayerOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };

void initVideoPlayerOverlay(GfxTexture spritesheet)
{
   sprites = spritesheet;
   font    = openSystemFont(FONT_POP);

   initLabel(&messageLabel,   &font, 0, 0, 1200, AUTO, MESSAGE_SIZE,   COLOR_MESSAGE,  TEXT_WRAP,  "");
   initLabel(&statusLabel,    &font, 0, 0, 600,  AUTO, MESSAGE_SIZE,   COLOR_STATUS,   TEXT_NOWRAP, "Loading...");
   initLabel(&nameLabel,      &font, NAME_X, NAME_Y, NAME_MAX_WIDTH, AUTO, NAME_SIZE, COLOR_MESSAGE, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&timeLeftLabel,  &font, 0, 0, 200,  AUTO, TIME_SIDE_SIZE, COLOR_TIME_DIM, TEXT_NOWRAP, "");
   initLabel(&timeRightLabel, &font, 0, 0, 200,  AUTO, TIME_SIDE_SIZE, COLOR_TIME_DIM, TEXT_NOWRAP, "");

   initLabel(&volNumLabel, &font, 0, 0, 80, AUTO, VOL_NUM_SIZE, COLOR_MESSAGE, TEXT_NOWRAP, "");

   initImage(&thumbImg,   sprites, 0, 0, THUMB_DIA, THUMB_DIA, spriteRegions[SPRITE_BLUE_CIRCLE], GFX_FILTER_LINEAR);
   initImage(&speakerImg, sprites, 0, 0, VOL_SPEAKER_W, VOL_SPEAKER_H, spriteRegions[SPRITE_SPEAKER], GFX_FILTER_LINEAR);
   initImage(&pillBlue,   sprites, 0, 0, VOL_PILL_W, VOL_PILL_H, spriteRegions[SPRITE_PILL],      GFX_FILTER_LINEAR);
   initImage(&pillGrey,   sprites, 0, 0, VOL_PILL_W, VOL_PILL_H, spriteRegions[SPRITE_PILL_GREY], GFX_FILTER_LINEAR);
   initSlice(&trackSlice, sprites, 0, 0, 100, BAR_H, spriteRegions[SPRITE_PILL_GREY], BAR_CAP);
   initSlice(&fillSlice,  sprites, 0, 0, 100, BAR_H, spriteRegions[SPRITE_PILL],      BAR_CAP);
   ready = 1;
}

static void layoutPlayer(void)
{
   int w = state.screenW, h = state.screenH;
   state.centerX = w / 2;
   state.centerY = h / 2;
   state.barLeft  = (int)(w * 0.24f);
   state.barRight = w - state.barLeft;
   state.barY     = (int)(h * 0.94f);   // low on the screen, per the design
   state.volPillX   = (int)(w * 0.07f);
   state.volBottomY = state.centerY + (VOLUME_PILLS - 1) * VOL_PILL_PITCH / 2;   // column centred vertically
}

// ============================================================================
// open / lifecycle
// ============================================================================

static void worker(uint64_t arg)
{
   (void)arg;
   probeVideo(state.path, &state.pending);
   if (state.pending.verdict == VIDEO_PLAYABLE)
      state.pendingPlayer = createVideoPlayer(state.path, allocGfxVideoBuffer, freeGfxVideoBuffer);
   __sync_synchronize();
   state.workerDone = 1;
   exitThread();
}

static void joinWorker(void)
{
   if (state.threadActive) { joinThread(state.workerTid); state.threadActive = 0; }
}

static void releasePlayback(void)
{
   if (state.player || state.pendingPlayer) finishGfx();   // RSX may still be sampling a frame buffer
   if (state.player) { destroyVideoPlayer(state.player); state.player = 0; }
   if (state.pendingPlayer) { destroyVideoPlayer(state.pendingPlayer); state.pendingPlayer = 0; }
}

// (re)opens the video at `videoPath` in the already-open overlay: tears down the current playback,
// resets per-file state (volume persists), and kicks the background probe+open worker.
static void startVideo(const char *videoPath)
{
   joinWorker();
   releasePlayback();

   strCopy(state.name, sizeof state.name, getBaseName(videoPath));
   strCopy(state.path, sizeof state.path, videoPath);
   setLabelText(&nameLabel, state.name);
   setLabelText(&messageLabel, "");
   state.haveResult = 0;
   state.workerDone = 0;
   state.seeking = 0;
   state.controlsShownUs = 0;
   state.volumeShownUs = 0;
   lastElapsed = lastRemain = -1;

   state.working      = 1;
   state.threadActive = (spawnJoinableThread(&state.workerTid, worker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "video-open") == 0);
   if (!state.threadActive) {             // spawn failed: open synchronously
      probeVideo(state.path, &state.pending);
      if (state.pending.verdict == VIDEO_PLAYABLE)
         state.pendingPlayer = createVideoPlayer(state.path, allocGfxVideoBuffer, freeGfxVideoBuffer);
      state.workerDone = 1;
   }
}

int openVideoPlayer(const char *videoPath)
{
   if (!isVideoFile(videoPath)) return -1;

   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   layoutPlayer();

   // scan the folder for sibling videos so L1/R1 can step through them
   playlistOpen(&state.playlist, videoPath, isVideoFile);

   startVideo(videoPath);
   showOverlay(&videoPlayerOverlay);
   return 0;
}

static void show(void) { videoPlayerOverlay.status = OVERLAY_VISIBLE; }

static void hide(void)
{
   joinWorker();
   releasePlayback();
   videoPlayerOverlay.status = OVERLAY_HIDDEN;
}

static void term(void)
{
   joinWorker();
   releasePlayback();
   if (ready) {
      freeLabel(&messageLabel);
      freeLabel(&statusLabel);
      freeLabel(&nameLabel);
      freeLabel(&timeLeftLabel);
      freeLabel(&timeRightLabel);
      freeLabel(&volNumLabel);
      closeFont(&font);
      ready = 0;
   }
   memset(&state, 0, sizeof state);
   videoPlayerOverlay.status = OVERLAY_TERMINATED;
}

static void adoptResult(void)
{
   __sync_synchronize();
   joinWorker();
   state.working    = 0;
   state.result     = state.pending;
   state.haveResult = 1;

   if (state.result.verdict == VIDEO_PLAYABLE && state.pendingPlayer) {
      state.player = state.pendingPlayer;
      state.pendingPlayer = 0;
      setAudioPcmFeedVolume((float)volumeLevel / VOLUME_PILLS);
      state.controlsShownUs = sys_time_get_system_time();   // show the bar + name briefly at start
      logInfo("[video-player] playing: %s\n", state.name);
   } else if (state.result.verdict != VIDEO_PLAYABLE) {
      setLabelText(&messageLabel, state.result.reason);
      logInfo("[video-player] not playable: %s - %s\n", state.name, state.result.reason);
   } else {
      setLabelText(&messageLabel, "Couldn't start playback.");   // playable but the engine failed to open
   }
}

// ============================================================================
// input
// ============================================================================

static void changeVolume(int delta)
{
   int level = volumeLevel + delta;
   if (level < 0) level = 0;
   if (level > VOLUME_PILLS) level = VOLUME_PILLS;
   volumeLevel = level;
   setAudioPcmFeedVolume((float)level / VOLUME_PILLS);
   state.volumeShownUs = sys_time_get_system_time();   // (re)show the meter
}

static void showControls(void) { state.controlsShownUs = sys_time_get_system_time(); }

// steps to the prev/next sibling video in the folder, wrapping at the ends.
static void stepVideo(int delta)
{
   char full[MAX_PATH_LEN];
   playlistStep(&state.playlist, delta, full, sizeof full);
   startVideo(full);
}

// accumulates a scrub target while left/right is pressed; the engine seek fires once input goes quiet
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

static void update(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { hideOverlay(&videoPlayerOverlay); return; }

   if (state.working) { if (state.workerDone) adoptResult(); return; }

   if (state.playlist.count > 1) {
      if (isPadButtonPressed(PAD_BTN_L1)) { stepVideo(-1); return; }
      if (isPadButtonPressed(PAD_BTN_R1)) { stepVideo(+1); return; }
   }
   if (!state.player) return;

   uint64_t nowUs = sys_time_get_system_time();

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (isVideoEnded(state.player)) seekVideoPlayer(state.player, 0.0f);   // ended: restart from the top
      else setVideoPaused(state.player, !isVideoPaused(state.player));
      showControls();
   }

   // volume: stepped, with auto-repeat while held
   static ButtonRepeat volumeRepeat;
   if (isRepeatDue(&volumeRepeat, getPadButtonState(PAD_BTN_UP)))        changeVolume(+1);
   else if (isRepeatDue(&volumeRepeat, getPadButtonState(PAD_BTN_DOWN))) changeVolume(-1);

   // seek: left/right scrub the target; the jump applies when the input goes quiet
   static ButtonRepeat seekRepeat;
   if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_RIGHT)))     nudgeSeek(+SEEK_STEP_SECONDS, nowUs);
   else if (isRepeatDue(&seekRepeat, getPadButtonState(PAD_BTN_LEFT))) nudgeSeek(-SEEK_STEP_SECONDS, nowUs);

   if (state.seeking && nowUs - state.lastSeekInputUs >= SEEK_APPLY_IDLE_US) {
      seekVideoPlayer(state.player, state.seekTarget);
      state.seeking = 0;
   }
}

// ============================================================================
// draw
// ============================================================================

static void formatHMS(char *buffer, int cap, int seconds)
{
   if (seconds < 0) seconds = 0;
   snprintf(buffer, cap, "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
}

static void syncTimeLabels(void)
{
   float duration = getVideoDurationSeconds(state.player);
   float position = state.seeking ? state.seekTarget : getVideoPositionSeconds(state.player);
   int elapsed = (int)position;
   int remain  = (int)(duration - position + 0.5f);
   if (remain < 0) remain = 0;

   if (elapsed != lastElapsed) { char text[16]; formatHMS(text, sizeof text, elapsed); setLabelText(&timeLeftLabel, text); lastElapsed = elapsed; }
   if (remain  != lastRemain)  { char text[16]; formatHMS(text, sizeof text, remain);  setLabelText(&timeRightLabel, text); lastRemain = remain; }
}

static void drawSeekBar(void)
{
   float duration = getVideoDurationSeconds(state.player);
   float position = state.seeking ? state.seekTarget : getVideoPositionSeconds(state.player);
   float progress = duration > 0.0f ? position / duration : 0.0f;
   if (progress < 0.0f) progress = 0.0f;
   if (progress > 1.0f) progress = 1.0f;

   int span    = state.barRight - state.barLeft;
   int barTopY = state.barY - BAR_H / 2;
   int filledW = (int)(progress * span);

   trackSlice.x = state.barLeft; trackSlice.y = barTopY; trackSlice.w = span;
   drawSlice(&trackSlice);
   if (filledW >= BAR_CAP * 2) { fillSlice.x = state.barLeft; fillSlice.y = barTopY; fillSlice.w = filledW; drawSlice(&fillSlice); }
   drawImageAt(&thumbImg, state.barLeft + filledW - THUMB_DIA / 2, state.barY - THUMB_DIA / 2);

   syncTimeLabels();
   drawLabelAt(&timeLeftLabel,  state.barLeft - SIDE_TIME_GAP - timeLeftLabel.tt.tex.w, state.barY - timeLeftLabel.tt.tex.h / 2);
   drawLabelAt(&timeRightLabel, state.barRight + SIDE_TIME_GAP, state.barY - timeRightLabel.tt.tex.h / 2);
}

static void drawVolumeMeter(void)
{
   uint64_t shownFor = sys_time_get_system_time() - state.volumeShownUs;
   if (state.volumeShownUs == 0 || shownFor >= VOLUME_VISIBLE_US) return;

   if (volumeLevel != lastVolNum) {
      char num[8];
      snprintf(num, sizeof num, "%d", volumeLevel);
      setLabelText(&volNumLabel, num);
      lastVolNum = volumeLevel;
   }

   // pills bottom-up: the lowest `volumeLevel` are blue (filled), the rest grey
   for (int i = 0; i < VOLUME_PILLS; i++) {
      int y = state.volBottomY - i * VOL_PILL_PITCH;
      drawImageAt(i < volumeLevel ? &pillBlue : &pillGrey, state.volPillX, y);
   }

   int colCenterX = state.volPillX + VOL_PILL_W / 2;
   int topPillY   = state.volBottomY - (VOLUME_PILLS - 1) * VOL_PILL_PITCH;
   drawLabelAt(&volNumLabel, colCenterX - volNumLabel.tt.tex.w / 2, topPillY - VOL_NUM_SIZE - 14);
   drawImageAt(&speakerImg, colCenterX - VOL_SPEAKER_W / 2, state.volBottomY + VOL_PILL_H + VOL_SPEAKER_GAP);
}

// fits w x h inside the screen preserving aspect ratio; returns the centred destination rect.
static void letterboxRect(int w, int h, int *dx, int *dy, int *dw, int *dh)
{
   float frameAspect  = (float)w / (float)h;
   float screenAspect = (float)state.screenW / (float)state.screenH;
   if (frameAspect > screenAspect) { *dw = state.screenW; *dh = (int)(state.screenW / frameAspect); }
   else                            { *dh = state.screenH; *dw = (int)(state.screenH * frameAspect); }
   *dx = (state.screenW - *dw) / 2;
   *dy = (state.screenH - *dh) / 2;
}

// controls (seek bar + filename) stay up while paused / scrubbing / at the end, else auto-hide
static int controlsVisible(void)
{
   if (isVideoPaused(state.player) || isVideoEnded(state.player) || state.seeking) return 1;
   return state.controlsShownUs != 0 && sys_time_get_system_time() - state.controlsShownUs < CONTROLS_VISIBLE_US;
}

static void drawNameCaption(void)
{
   if (nameLabel.tt.tex.w <= 0) return;
   fillGfxRectangle(NAME_X - CAPTION_PAD_X, NAME_Y - CAPTION_PAD_Y,
                    nameLabel.tt.tex.w + CAPTION_PAD_X * 2, nameLabel.tt.tex.h + CAPTION_PAD_Y * 2,
                    COLOR_CAPTION_BG);
   drawLabel(&nameLabel);
}

static void draw(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, COLOR_SCRIM);

   if (state.player) {
      // the due frame draws zero-copy: the RSX samples the decoder's YUV planes in place
      int w, h;
      const uint8_t *frame = getVideoFrame(state.player, &w, &h);
      if (frame) {
         int dx, dy, dw, dh;
         letterboxRect(w, h, &dx, &dy, &dw, &dh);
         drawGfxYuvFrame(dx, dy, dw, dh, frame, w, h);
      }
      if (controlsVisible()) {
         drawSeekBar();
         drawNameCaption();
      }
      drawVolumeMeter();
      return;
   }

   if (!state.haveResult) {
      drawLabelAt(&statusLabel, state.centerX - statusLabel.tt.tex.w / 2, state.centerY - MESSAGE_SIZE / 2);
      return;
   }

   drawLabelAt(&messageLabel, state.centerX - messageLabel.tt.tex.w / 2, state.centerY - messageLabel.tt.tex.h / 2);
}
