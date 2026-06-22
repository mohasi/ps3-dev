// audio-player-overlay - full-screen player for a single audio file (wav, ogg, mp3 or flac).
// Streams the file through the audio mixer and draws the player UI on top of the home screen:
// the file icon + name, a live waveform, a seek bar with elapsed/remaining times, and a left-edge
// volume meter. Cross toggles play/pause, the D-pad left/right seeks (holding accelerates),
// up/down changes volume, and Circle closes.
//
// Loading runs on a background thread so the overlay appears instantly and the UI never freezes on
// a large file; the icon + name show right away with a "Loading..." note, then playback and the
// waveform swap in once the worker finishes.
//
// The waveform needs no custom shader: bars are thin rounded pills (rect + circle caps) whose
// alpha is faded toward the left/right edges via the stock pipeline's per-vertex colour.
#include "overlays/audio-player-overlay.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "audio.h"
#include "colors.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/slice.h"
#include "button-repeat.h"
#include "thread.h"             // spawnJoinableThread, joinThread
#include "dir-playlist.h"       // folder scan + prev/next-with-wrap navigation
#include "vfs.h"               // getBaseName, MAX_PATH_LEN
#include "string-utilities.h"   // strCopy
#include "sprite-regions.h"
#include "dbg.h"                // logError

#include <string.h>
#include <stdio.h>              // snprintf (app side; not a VSH PRX)
#include <sys/sys_time.h>

#define COLOR_SCRIM        0xF2000000u   // near-black backdrop
#define COLOR_WAVE_RGB     0x0060A5FAu   // blue-400, RGB only (alpha is added per bar)
#define WAVE_PEAK_ALPHA    0xF0          // a bar's alpha at full strength, before the edge fade
#define COLOR_NAME         0xFFFFFFFFu
#define COLOR_SUBTITLE     0x80FFFFFFu   // track title under the filename, 50% white
#define COLOR_STATUS       0xB0FFFFFFu   // "Loading..." note
#define COLOR_TIME         0xFFFFFFFFu
#define COLOR_TIME_DIM     0x99FFFFFFu

#define WAVE_BARS          96            // thin bars with clear gaps between them
#define WAVE_EDGE_FADE     0.18f         // fraction of each side the bars fade in over (bigger = wider fade)
#define VOLUME_PILLS       15            // meter height in pills; also the max volume level
#define VOLUME_DEFAULT     10            // starting level the first time the player opens
#define VOLUME_VISIBLE_US  2500000ULL    // meter auto-hides this long after the last change

// seek-by-hold: a single tap nudges by a small fixed amount; holding ramps the rate from MIN to MAX
// along a squared curve (gentle and granular at first, accelerating the longer it's held).
#define SEEK_PRESS_SECS    1.0f          // a single press steps ~1 second
#define SEEK_RATE_MIN      3.0f          // seconds of audio per real second, at hold start
#define SEEK_RATE_MAX      200.0f        // ...ramped up to this
#define SEEK_RAMP_US       2000000.0f    // reach full speed after ~2s of holding

// text sizes
#define NAME_SIZE          30
#define SUBTITLE_SIZE      26
#define SUBTITLE_GAP       16            // below the filename
#define TIME_CENTER_SIZE   26
#define TIME_SIDE_SIZE     20
#define VOL_NUM_SIZE       22

// icon (top, centred)
#define ICON_W             84
#define ICON_H             100

// seek bar geometry
#define BAR_H              10
#define BAR_CAP            5             // rounded-end cap of the 31px pill sprite
#define THUMB_DIA          28
#define SIDE_TIME_GAP      24            // gap between the bar end and its side time label

// volume meter geometry
#define VOL_PILL_W         31
#define VOL_PILL_H         10
#define VOL_PILL_PITCH     19            // pill height + vertical gap
#define VOL_SPEAKER_W      32
#define VOL_SPEAKER_H      29
#define VOL_SPEAKER_GAP    22            // gap below the lowest pill to the speaker glyph

static struct {
   int screenW, screenH;

   Audio audio;
   int   loaded;                 // 1 once a clip is decoded and playing/paused
   char  name[256];

   uint64_t volumeShownUs;       // last volume change, for the meter auto-hide

   uint64_t lastUpdateUs;        // previous frame time, for dt
   uint64_t seekHeldUs;          // accumulated hold time on the active seek direction
   int      seekDir;             // -1 / 0 / +1: direction held last frame (resets ramp on change)
   int      seekMuted;           // 1 while audio is muted during an active seek (silent scrub)
   float    seekTarget;          // the position we're scrubbing toward (owned here, not read back from
                          // the mixer, so the bar moves smoothly and doesn't fight decode latency)

   float waveBars[WAVE_BARS];    // smoothed 0..1 amplitudes, left = oldest

   // layout, computed from the screen size on open
   int centerX;
   int iconX, iconY, nameY;
   int waveLeft, waveRight, waveCenterY, waveMaxH;
   int timeCenterY;
   int barLeft, barRight, barY;
   int volPillX, volBottomY;     // left edge of pills; top-y of the lowest pill

   DirPlaylist playlist;   // sibling playable tracks in the folder, for L1/R1 navigation

   // background decode: loadSfx runs on a worker so a big file doesn't freeze the UI
   int          loading;         // 1 while the worker is decoding
   int          threadActive;    // 1 while decodeTid is still joinable
   volatile int decodeDone;      // worker sets this once pendingAudio is ready
   Audio        pendingAudio;    // clip handed back by the worker, adopted on the main thread
   char         pendingPath[MAX_PATH_LEN];
   sys_ppu_thread_t decodeTid;
} state;

// volume level (0..VOLUME_PILLS) kept outside `state` so it survives re-opens and the overlay's
// term(); persists for the app's lifetime and resets to VOLUME_DEFAULT when the file manager exits.
static int volumeLevel = VOLUME_DEFAULT;

// sprite-sheet UI pieces (lazy-initialised in init, reused across opens)
static GfxTexture sprites;
static Image  iconImg, thumbImg, speakerImg, pillBlue, pillGrey;
static Slice  trackSlice, fillSlice;

// fonts/labels (kept across re-opens; their text textures grow as needed)
static Font  font;
static int   ready;
static Label nameLabel, subtitleLabel, statusLabel, timeCenterLabel, timeLeftLabel, timeRightLabel, volNumLabel;

// last values the time labels were rendered at, so we only re-rasterise on change
static int lastElapsed = -1, lastRemain = -1, lastTotal = -1, lastVolNum = -1;

// forward decls (referenced by the Overlay table; defined below)
static void show(void);
static void hide(void);
static void update(void);
static void draw(void);
static void term(void);
static void sampleWaveform(void);   // defined in the waveform section, called from update()
static void decodeWorker(uint64_t arg);   // background loadSfx; defined below
static void startTrack(const char *audioPath);   // (re)loads one track into the open overlay
static void stepTrack(int delta);                 // L1/R1: move to the prev/next sibling track

Overlay audioPlayerOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };

// ============================================================================
// setup
// ============================================================================

void initAudioPlayerOverlay(GfxTexture spritesheet)
{
   sprites = spritesheet;
   font    = openSystemFont(FONT_POP);

   initImage(&iconImg,    sprites, 0, 0, ICON_W,        ICON_H,        spriteRegions[SPRITE_AUDIO],       GFX_FILTER_LINEAR);
   initImage(&thumbImg,   sprites, 0, 0, THUMB_DIA,     THUMB_DIA,     spriteRegions[SPRITE_BLUE_CIRCLE], GFX_FILTER_LINEAR);
   initImage(&speakerImg, sprites, 0, 0, VOL_SPEAKER_W, VOL_SPEAKER_H, spriteRegions[SPRITE_SPEAKER],     GFX_FILTER_LINEAR);
   initImage(&pillBlue,   sprites, 0, 0, VOL_PILL_W,    VOL_PILL_H,    spriteRegions[SPRITE_PILL],        GFX_FILTER_LINEAR);
   initImage(&pillGrey,   sprites, 0, 0, VOL_PILL_W,    VOL_PILL_H,    spriteRegions[SPRITE_PILL_GREY],   GFX_FILTER_LINEAR);

   // both seek-bar slices keep fixed UVs (set from the pill sprite); only x/width change per frame
   initSlice(&trackSlice, sprites, 0, 0, 100, BAR_H, spriteRegions[SPRITE_PILL_GREY], BAR_CAP);
   initSlice(&fillSlice,  sprites, 0, 0, 100, BAR_H, spriteRegions[SPRITE_PILL],      BAR_CAP);

   initLabel(&nameLabel,       &font, 0, 0, 1400, AUTO, NAME_SIZE,        COLOR_NAME,     TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&subtitleLabel,   &font, 0, 0, 1400, AUTO, SUBTITLE_SIZE,    COLOR_SUBTITLE, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&statusLabel,     &font, 0, 0, 400,  AUTO, TIME_CENTER_SIZE, COLOR_STATUS,   TEXT_NOWRAP,          "Loading...");
   initLabel(&timeCenterLabel, &font, 0, 0, 600,  AUTO, TIME_CENTER_SIZE, COLOR_TIME,     TEXT_NOWRAP,          "");
   initLabel(&timeLeftLabel,   &font, 0, 0, 200,  AUTO, TIME_SIDE_SIZE,   COLOR_TIME_DIM, TEXT_NOWRAP,          "");
   initLabel(&timeRightLabel,  &font, 0, 0, 200,  AUTO, TIME_SIDE_SIZE,   COLOR_TIME_DIM, TEXT_NOWRAP,          "");
   initLabel(&volNumLabel,     &font, 0, 0, 80,   AUTO, VOL_NUM_SIZE,     COLOR_WHITE,    TEXT_NOWRAP,          "");
   ready = 1;
}

// positions every element relative to the current screen size, matching the mockup's proportions.
static void layoutPlayer(void)
{
   int w = state.screenW, h = state.screenH;
   state.centerX = w / 2;

   state.iconX = state.centerX - ICON_W / 2;
   state.iconY = (int)(h * 0.16f);
   state.nameY = state.iconY + ICON_H + 18;

   state.waveLeft    = (int)(w * 0.17f);
   state.waveRight   = w - state.waveLeft;
   state.waveCenterY = h / 2;                  // visualisation centred on the screen
   state.waveMaxH    = (int)(h * 0.12f);

   state.timeCenterY = (int)(h * 0.70f);       // time + seek bar sit below the visualisation

   state.barLeft  = (int)(w * 0.24f);
   state.barRight = w - state.barLeft;
   state.barY     = (int)(h * 0.75f);

   state.volPillX   = (int)(w * 0.07f);
   int stackHeight  = VOLUME_PILLS * VOL_PILL_PITCH - (VOL_PILL_PITCH - VOL_PILL_H);
   int stackTop     = h / 2 - stackHeight / 2;
   state.volBottomY = stackTop + stackHeight - VOL_PILL_H;   // top-y of the lowest pill
}

// ============================================================================
// open / lifecycle
// ============================================================================

// background decode entry: loads the clip off the main thread, then flags it ready. The barrier
// ensures pendingAudio's fields are visible before the main thread sees decodeDone.
static void decodeWorker(uint64_t arg)
{
   (void)arg;
   // stream everything: a wav is read from disk on demand (never fully loaded), an ogg decodes from
   // its compressed bytes. Both open instantly and the lib tracks position/duration + a waveform.
   state.pendingAudio = loadSfx(state.pendingPath, SFX_STREAM);
   __sync_synchronize();
   state.decodeDone = 1;
   exitThread();
}

// joins the decode worker if it's still tracked (safe to call more than once).
static void joinDecode(void)
{
   if (state.threadActive) { joinThread(state.decodeTid); state.threadActive = 0; }
}

int openAudioPlayer(const char *audioPath)
{
   if (!isPlayableAudioFile(audioPath)) return -1;

   state.screenW = getGfxScreenWidth();
   state.screenH = getGfxScreenHeight();
   layoutPlayer();

   // scan the folder for sibling playable tracks so L1/R1 can step through them
   playlistOpen(&state.playlist, audioPath, isPlayableAudioFile);

   startTrack(audioPath);
   showOverlay(&audioPlayerOverlay);
   return 0;
}

static void show(void) { audioPlayerOverlay.status = OVERLAY_VISIBLE; }

// releases both the playing clip and any not-yet-adopted decode result, after waiting out the
// worker so it can't write into freed state.
static void releaseAudio(void)
{
   joinDecode();
   freeSfx(&state.pendingAudio);   // frees a decoded-but-never-adopted handle (no-op when zeroed)
   if (state.loaded) { stopSfx(&state.audio); freeSfx(&state.audio); }
   memset(&state.audio, 0, sizeof state.audio);
   memset(&state.pendingAudio, 0, sizeof state.pendingAudio);
   state.loaded = 0; state.loading = 0; state.decodeDone = 0;
}

// (re)loads the track at `audioPath` into the already-open overlay: tears down the current clip,
// resets per-track state (volume persists), and kicks the background decode worker.
static void startTrack(const char *audioPath)
{
   releaseAudio();

   strCopy(state.name, sizeof state.name, getBaseName(audioPath));
   strCopy(state.pendingPath, sizeof state.pendingPath, audioPath);
   setLabelText(&nameLabel, state.name);
   setLabelText(&subtitleLabel, "");   // cleared until tags are read; avoids showing a stale title

   state.decodeDone = 0;
   state.volumeShownUs = 0;            // meter hidden until the user touches it
   state.seekDir = 0; state.seekHeldUs = 0; state.seekMuted = 0;
   state.lastUpdateUs = sys_time_get_system_time();
   memset(state.waveBars, 0, sizeof state.waveBars);
   lastElapsed = lastRemain = lastTotal = lastVolNum = -1;

   // decode on a worker so the overlay stays responsive and never freezes on a big file
   state.loading      = 1;
   state.threadActive = (spawnJoinableThread(&state.decodeTid, decodeWorker, 0,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "audio-decode") == 0);
   if (!state.threadActive) {           // spawn failed: load synchronously so it still plays
      state.pendingAudio = loadSfx(state.pendingPath, SFX_STREAM);
      state.decodeDone   = 1;
   }
}

// steps to the prev/next sibling track in the folder, wrapping at the ends.
static void stepTrack(int delta)
{
   char full[MAX_PATH_LEN];
   playlistStep(&state.playlist, delta, full, sizeof full);
   startTrack(full);
}

static void hide(void)
{
   releaseAudio();   // hold no PCM while back in the file list
   audioPlayerOverlay.status = OVERLAY_HIDDEN;
}

static void term(void)
{
   releaseAudio();
   if (ready) {
      freeLabel(&nameLabel);
      freeLabel(&subtitleLabel);
      freeLabel(&statusLabel);
      freeLabel(&timeCenterLabel);
      freeLabel(&timeLeftLabel);
      freeLabel(&timeRightLabel);
      freeLabel(&volNumLabel);
      closeFont(&font);
      ready = 0;
   }
   memset(&state, 0, sizeof state);
   audioPlayerOverlay.status = OVERLAY_TERMINATED;
}

// ============================================================================
// input
// ============================================================================

static void togglePlayPause(void)
{
   switch (state.audio.state) {
      case SFX_STATE_PLAYING: pauseSfx(&state.audio);  break;
      case SFX_STATE_PAUSED:  resumeSfx(&state.audio); break;
      default:  // ended/stopped: restart from the beginning
         playSfx(&state.audio, (float)volumeLevel / VOLUME_PILLS, 1.0f, 0);
         break;
   }
}

static void changeVolume(int delta)
{
   int level = volumeLevel + delta;
   if (level < 0) level = 0;
   if (level > VOLUME_PILLS) level = VOLUME_PILLS;
   volumeLevel = level;
   setSfxVolume(&state.audio, (float)level / VOLUME_PILLS);
   state.volumeShownUs = sys_time_get_system_time();   // (re)show the meter
}

// applies a seek for the held direction this frame. a fresh press jumps a fixed amount; holding
// ramps the scrub rate up over time. dtUs is the time since the previous frame.
static void handleSeek(int dir, uint64_t dtUs)
{
   float duration = getSfxDurationSeconds(&state.audio);

   // on the first frame of a seek (direction just changed), anchor the target at the live position;
   // after that we advance our own target rather than re-reading the (decode-lagged) playback position.
   if (dir != state.seekDir) {
      state.seekDir = dir;
      state.seekHeldUs = 0;
      state.seekTarget = getSfxPositionSeconds(&state.audio);
   }

   PadButtonState buttonState = getPadButtonState(dir > 0 ? PAD_BTN_RIGHT : PAD_BTN_LEFT);
   float delta;
   if (buttonState == PAD_BUTTON_STATE_PRESSED) {
      delta = SEEK_PRESS_SECS;
   } else {
      state.seekHeldUs += dtUs;
      float ramp = state.seekHeldUs / SEEK_RAMP_US;
      if (ramp > 1.0f) ramp = 1.0f;
      float rate = SEEK_RATE_MIN + (SEEK_RATE_MAX - SEEK_RATE_MIN) * ramp * ramp;   // squared ramp
      delta = rate * (float)dtUs / 1000000.0f;
   }

   state.seekTarget += (dir > 0 ? delta : -delta);
   if (state.seekTarget < 0.0f) state.seekTarget = 0.0f;
   if (state.seekTarget > duration) state.seekTarget = duration;
   seekSfx(&state.audio, state.seekTarget);
}

// position to show in the UI: while actively seeking, show where we're scrubbing to (smooth, our own
// target); otherwise show the real playback position.
static float displayPosSeconds(void)
{
   return state.seekMuted ? state.seekTarget : getSfxPositionSeconds(&state.audio);
}

static void update(void)
{
   uint64_t now  = sys_time_get_system_time();
   uint64_t dtUs = now - state.lastUpdateUs;
   state.lastUpdateUs = now;

   // Circle always closes (hide() joins the worker first, so a load in flight is cancelled cleanly)
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) { hideOverlay(&audioPlayerOverlay); return; }

   // L1/R1 step to the previous/next track in the folder (wrapping at the ends)
   if (state.playlist.count > 1) {
      if (isPadButtonPressed(PAD_BTN_L1)) { stepTrack(-1); return; }
      if (isPadButtonPressed(PAD_BTN_R1)) { stepTrack(+1); return; }
   }

   // still decoding: keep showing the "Loading..." note; adopt the clip and start playing once ready
   if (state.loading) {
      if (!state.decodeDone) return;
      __sync_synchronize();
      joinDecode();
      state.loading = 0;
      state.audio   = state.pendingAudio;
      memset(&state.pendingAudio, 0, sizeof state.pendingAudio);
      // a valid handle has PCM (memory), or a stream decoder/file (ogg, mp3, flac, or wav)
      state.loaded  = (state.audio.pcmData || state.audio.vorbis ||
                       state.audio.mp3 || state.audio.flac || state.audio.wav);
      if (!state.loaded) { logError("[audio-player] decode failed: %s\n", state.pendingPath); return; }
      setLabelText(&subtitleLabel, state.audio.title);   // track title from tags (empty if none)
      state.lastUpdateUs = now;   // don't count the decode wait as a seek dt
      playSfx(&state.audio, (float)volumeLevel / VOLUME_PILLS, 1.0f, 0);
      return;
   }

   if (isPadButtonPressed(PAD_BTN_CROSS))  togglePlayPause();

   // volume: stepped, with auto-repeat while held
   static ButtonRepeat volumeRepeat;
   if (isRepeatDue(&volumeRepeat, getPadButtonState(PAD_BTN_UP)))        changeVolume(+1);
   else if (isRepeatDue(&volumeRepeat, getPadButtonState(PAD_BTN_DOWN))) changeVolume(-1);

   // seek: left/right, accelerating while held
   int seeking = 1;
   if (isPadButtonHeld(PAD_BTN_RIGHT) || getPadButtonState(PAD_BTN_RIGHT) == PAD_BUTTON_STATE_PRESSED)
      handleSeek(+1, dtUs);
   else if (isPadButtonHeld(PAD_BTN_LEFT) || getPadButtonState(PAD_BTN_LEFT) == PAD_BUTTON_STATE_PRESSED)
      handleSeek(-1, dtUs);
   else {
      state.seekDir = 0;
      seeking = 0;
   }

   // silent scrub: mute while actively seeking so you don't hear the playback jumping around,
   // then restore the user's volume on release.
   if (seeking && !state.seekMuted) { setSfxVolume(&state.audio, 0.0f); state.seekMuted = 1; }
   else if (!seeking && state.seekMuted) { setSfxVolume(&state.audio, (float)volumeLevel / VOLUME_PILLS); state.seekMuted = 0; }

   sampleWaveform();   // pull the latest amplitude envelope from the mixer
}

// ============================================================================
// waveform
// ============================================================================

// pulls the mixer's rolling amplitude envelope (works for both wav and streamed ogg) and eases
// the bars toward it; light smoothing keeps the motion from flickering. Frozen when paused, since
// the mixer stops updating the envelope for a non-playing stream.
static void sampleWaveform(void)
{
   float bins[WAVE_BARS] = {0};
   int n = getSfxWaveform(&state.audio, bins, WAVE_BARS);
   for (int i = 0; i < WAVE_BARS; i++) {
      float target = i < n ? bins[i] : 0.0f;
      state.waveBars[i] += (target - state.waveBars[i]) * 0.4f;
   }
}

// one thin bar mirrored about the centre line: a single rect through the centre with rounded tips at
// the top and bottom, matching the mockup. colour is 0xAARRGGBB (alpha included).
static void drawWaveBar(int cx, int cy, int halfH, int barW, uint32_t color)
{
   int radius = barW / 2;
   if (halfH < radius) halfH = radius;   // floor so a quiet bar is a small rounded dot, not a sliver
   fillGfxRectangle(cx - radius, cy - halfH, barW, halfH * 2, color);
   fillGfxCircle(cx, cy - halfH, radius, color);   // rounded top
   fillGfxCircle(cx, cy + halfH, radius, color);   // rounded bottom
}

static void drawWaveform(void)
{
   int span     = state.waveRight - state.waveLeft;
   int barPitch = span / WAVE_BARS;
   int barW     = barPitch * 2 / 5;   // ~40% bar, ~60% gap, so the bars read as thin with clear gaps
   if (barW < 4) barW = 4;
   if (barW & 1) barW++;              // even width centres the cap circle cleanly
   int cy = state.waveCenterY;

   for (int i = 0; i < WAVE_BARS; i++) {
      int cx = state.waveLeft + i * barPitch + barPitch / 2;

      // edge fade: full alpha across the middle, tapering over the outer WAVE_EDGE_FADE of each side
      float t        = (float)i / (float)(WAVE_BARS - 1);
      float edgeDist = t < (1.0f - t) ? t : (1.0f - t);
      float edge     = edgeDist / WAVE_EDGE_FADE;
      if (edge > 1.0f) edge = 1.0f;

      int halfH = (int)(state.waveBars[i] * state.waveMaxH);
      uint32_t color = ((uint32_t)(WAVE_PEAK_ALPHA * edge) << 24) | COLOR_WAVE_RGB;
      drawWaveBar(cx, cy, halfH, barW, color);
   }
}

// ============================================================================
// time + volume text
// ============================================================================

static void formatHMS(char *buf, int cap, int secs)
{
   if (secs < 0) secs = 0;
   snprintf(buf, cap, "%02d:%02d:%02d", secs / 3600, (secs % 3600) / 60, secs % 60);
}

static void formatSidedMS(char *buf, int cap, int secs, char sign)
{
   if (secs < 0) secs = 0;
   snprintf(buf, cap, "%c%d:%02d", sign, secs / 60, secs % 60);
}

// re-renders the time/volume labels only when their displayed value changes.
static void syncLabels(void)
{
   // total is rounded but elapsed is floored, and a finished stream sits a frame short of the full
   // length -- computing remain as total-elapsed then left it at +0:01 at the very end. Round the
   // remaining time off the real position so it lands on 0, and snap elapsed to total when done.
   float duration = getSfxDurationSeconds(&state.audio);
   float pos      = displayPosSeconds();
   if (pos > duration) pos = duration;
   int total   = (int)(duration + 0.5f);
   int elapsed = (int)pos;                       // floor: counts up like a stopwatch
   int remain  = (int)((duration - pos) + 0.5f); // round: lands on 0 exactly at the end
   if (remain <= 0) { remain = 0; elapsed = total; }   // finished: show the track fully elapsed
   if (elapsed > total) elapsed = total;

   if (elapsed != lastElapsed || total != lastTotal) {
      char elapsedStr[16], totalStr[16], combined[40];
      formatHMS(elapsedStr, sizeof elapsedStr, elapsed);
      formatHMS(totalStr,   sizeof totalStr,   total);
      snprintf(combined, sizeof combined, "%s / %s", elapsedStr, totalStr);
      setLabelText(&timeCenterLabel, combined);

      char left[16];
      formatSidedMS(left, sizeof left, elapsed, '-');
      setLabelText(&timeLeftLabel, left);
   }
   if (remain != lastRemain) {
      char right[16];
      formatSidedMS(right, sizeof right, remain, '+');
      setLabelText(&timeRightLabel, right);
   }
   if (volumeLevel != lastVolNum) {
      char num[8];
      snprintf(num, sizeof num, "%d", volumeLevel);
      setLabelText(&volNumLabel, num);
   }
   lastElapsed = elapsed; lastRemain = remain; lastTotal = total; lastVolNum = volumeLevel;
}

// ============================================================================
// draw
// ============================================================================

static void drawSeekBar(void)
{
   float duration = getSfxDurationSeconds(&state.audio);
   float pos      = displayPosSeconds();
   float progress = duration > 0.0f ? pos / duration : 0.0f;
   if (progress < 0.0f) progress = 0.0f;
   if (progress > 1.0f) progress = 1.0f;

   int span    = state.barRight - state.barLeft;
   int barTopY = state.barY - BAR_H / 2;
   int filledW = (int)(progress * span);
   int thumbX  = state.barLeft + filledW;

   // grey track, then the blue played portion on top (only once it can show both rounded caps)
   trackSlice.x = state.barLeft; trackSlice.y = barTopY; trackSlice.w = span;
   drawSlice(&trackSlice);
   if (filledW >= BAR_CAP * 2) {
      fillSlice.x = state.barLeft; fillSlice.y = barTopY; fillSlice.w = filledW;
      drawSlice(&fillSlice);
   }

   drawImageAt(&thumbImg, thumbX - THUMB_DIA / 2, state.barY - THUMB_DIA / 2);

   // elapsed time to the left of the bar (right-aligned to it), remaining to the right
   drawLabelAt(&timeLeftLabel,  state.barLeft - SIDE_TIME_GAP - timeLeftLabel.tt.tex.w,
                                state.barY - timeLeftLabel.tt.tex.h / 2);
   drawLabelAt(&timeRightLabel, state.barRight + SIDE_TIME_GAP, state.barY - timeRightLabel.tt.tex.h / 2);
}

static void drawVolumeMeter(void)
{
   uint64_t shownFor = sys_time_get_system_time() - state.volumeShownUs;
   if (state.volumeShownUs == 0 || shownFor >= VOLUME_VISIBLE_US) return;

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

static void draw(void)
{
   fillGfxRectangle(0, 0, state.screenW, state.screenH, COLOR_SCRIM);

   // icon + name show immediately, even while the clip is still decoding on the worker
   drawImageAt(&iconImg, state.iconX, state.iconY);
   drawLabelAt(&nameLabel, state.centerX - nameLabel.tt.tex.w / 2, state.nameY);

   // track title (from tags) as a centred subtitle under the filename, when present
   if (subtitleLabel.tt.tex.w > 0)
      drawLabelAt(&subtitleLabel, state.centerX - subtitleLabel.tt.tex.w / 2, state.nameY + NAME_SIZE + SUBTITLE_GAP);

   if (!state.loaded) {
      drawLabelAt(&statusLabel, state.centerX - statusLabel.tt.tex.w / 2, state.waveCenterY);
      return;
   }

   syncLabels();
   drawWaveform();

   // elapsed / total, centred above the seek bar
   drawLabelAt(&timeCenterLabel, state.centerX - timeCenterLabel.tt.tex.w / 2, state.timeCenterY);

   drawSeekBar();
   drawVolumeMeter();
}
