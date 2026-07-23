// progress-overlay - flat/metro modal driving a background file task (see file-task.h).
// shows a big percentage, a themed progress bar, and live stats derived from the byte progress plus a
// timer: size done/total, transfer speed, elapsed time and an estimated time remaining. all colours
// come from the active theme (see theme.h). circle cancels.
#include "overlays/progress-overlay.h"
#include "file-task.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "audio.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/console-glyphs.h"
#include "string-utilities.h"
#include "format.h"
#include "theme.h"
#include <sys/sys_time.h>

#define DIALOG_W       720
#define DIALOG_H       300
#define CONTENT_X      54   // left/right content margin

#define TITLE_Y        44
#define TITLE_SIZE     26
#define PERCENT_Y      36   // big percentage, right-aligned to the content edge
#define PERCENT_SIZE   40
#define SUBTITLE_Y     90
#define SUBTITLE_SIZE  18

#define BAR_Y          128
#define BAR_H          20

#define STATS_Y        164   // first stats line; the second sits STATS_GAP below
#define STATS_GAP      28
#define STATS_SIZE     17

// cancel hint (circle glyph + label, centered as a group along the bottom)
#define CANCEL_GLYPH   28
#define CANCEL_Y       242
#define CANCEL_GAP     10
#define CANCEL_SIZE    18

#define STAT_SEP       "   \xE2\x80\xA2   "   // "   •   ", the dot between stats

static Font   font;
static Audio *clickSfx;

static Image circleIcon;
static Label titleLabel, percentLabel, subtitleLabel, statsTop, statsBottom, cancelLabel;

static ProgressDoneCallback onDoneCb;
static int      cancelling;    // cancel requested, awaiting the worker to exit
static uint64_t startUs;       // task start, for elapsed / speed / remaining
static uint64_t lastStatsSec;  // last second the stats lines were rebuilt (throttle to 1 Hz)
static int      percent;

// busy (indeterminate) mode: no task, driven by the owner's polled callbacks (see startBusyProgress).
static int          busy;
static BusyStatusFn busyStatusFn;
static BusyDoneFn   busyDoneFn;
static void       (*busyCancelFn)(void);

void initProgressOverlay(Audio *sfx)
{
   clickSfx = sfx;
   font     = openSystemFont(FONT_POP);

   initGlyphIcon(&circleIcon, GLYPH_CIRCLE, CANCEL_GLYPH);

   int textW = DIALOG_W - CONTENT_X * 2;
   initLabel(&titleLabel,    &font, 0, 0, textW, AUTO, TITLE_SIZE,    activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&percentLabel,  &font, 0, 0, 160,   AUTO, PERCENT_SIZE,  activeTheme->textPrimary,   TEXT_NOWRAP,          "");
   initLabel(&subtitleLabel, &font, 0, 0, textW, AUTO, SUBTITLE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&statsTop,      &font, 0, 0, textW, AUTO, STATS_SIZE,    activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&statsBottom,   &font, 0, 0, textW, AUTO, STATS_SIZE,    activeTheme->textSecondary, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&cancelLabel,   &font, 0, 0, 120,   AUTO, CANCEL_SIZE,   activeTheme->textPrimary,   TEXT_NOWRAP,          "Cancel");
}

void startBusyProgress(const char *title, BusyStatusFn status, BusyDoneFn done,
                       void (*onCancel)(void), ProgressDoneCallback onDone)
{
   busy         = 1;
   busyStatusFn = status;
   busyDoneFn   = done;
   busyCancelFn = onCancel;
   onDoneCb     = onDone;
   cancelling   = 0;
   setLabelText(&titleLabel,    strOrEmpty(title));
   setLabelText(&subtitleLabel, "");
   showOverlay(&progressOverlay);
}

void startProgress(const char *title, const char *subtitle, TaskBody run, ProgressDoneCallback onDone)
{
   busy         = 0;
   onDoneCb     = onDone;
   cancelling   = 0;
   percent      = 0;
   startUs      = sys_time_get_system_time();
   lastStatsSec = (uint64_t)-1;   // force the first stats build
   setLabelText(&titleLabel,    strOrEmpty(title));
   setLabelText(&subtitleLabel, strOrEmpty(subtitle));
   setLabelText(&percentLabel,  "0%");
   setLabelText(&statsTop,      "");
   setLabelText(&statsBottom,   "");

   startTask(run);
   showOverlay(&progressOverlay);
}

// labels capture their colour at init, so a live theme switch needs this (the scrim/panel/bar read the
// theme live and follow for free). runs while the dialog is hidden; the next open renders in the new
// colour. see applyThemeToHome.
void rethemeProgressOverlay(void)
{
   setLabelColor(&titleLabel,    activeTheme->textPrimary);
   setLabelColor(&percentLabel,  activeTheme->textPrimary);
   setLabelColor(&subtitleLabel, activeTheme->textSecondary);
   setLabelColor(&statsTop,      activeTheme->textSecondary);
   setLabelColor(&statsBottom,   activeTheme->textSecondary);
   setLabelColor(&cancelLabel,   activeTheme->textPrimary);
}

static void show(void) { progressOverlay.status = OVERLAY_VISIBLE; }
static void hide(void) { progressOverlay.status = OVERLAY_HIDDEN; }

// "M:SS", or "H:MM:SS" past an hour.
static void formatDuration(uint64_t seconds, char *buf, int cap)
{
   uint64_t h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
   int o = 0;
   if (h > 0) {
      o = appendUint64(buf, cap, o, h);
      buf[o++] = ':';
      if (m < 10) buf[o++] = '0';
   }
   o = appendUint64(buf, cap, o, m);
   buf[o++] = ':';
   if (s < 10) buf[o++] = '0';
   o = appendUint64(buf, cap, o, s);
   buf[o] = '\0';
}

// rebuild the two stats lines from the current byte progress + elapsed time.
static void updateStats(uint64_t done, uint64_t total, uint64_t elapsedSec)
{
   char doneStr[24], totalStr[24], speedStr[24], line[96];
   uint64_t speed = elapsedSec > 0 ? done / elapsedSec : 0;

   // top: "342 MB of 1.4 GB   •   28 MB/s"
   formatSize(done, doneStr);
   formatSize(total, totalStr);
   formatSize(speed, speedStr);
   int o = 0;
   appendStr(line, sizeof line, &o, doneStr);
   appendStr(line, sizeof line, &o, " of ");
   appendStr(line, sizeof line, &o, totalStr);
   appendStr(line, sizeof line, &o, STAT_SEP);
   appendStr(line, sizeof line, &o, speedStr);
   appendStr(line, sizeof line, &o, "/s");
   line[o] = '\0';
   setLabelText(&statsTop, line);

   // bottom: "Elapsed 0:12   •   ~0:45 left" (or "estimating..." until there's a rate)
   char elapsedStr[16];
   formatDuration(elapsedSec, elapsedStr, sizeof elapsedStr);
   o = 0;
   appendStr(line, sizeof line, &o, "Elapsed ");
   appendStr(line, sizeof line, &o, elapsedStr);
   appendStr(line, sizeof line, &o, STAT_SEP);
   if (speed > 0 && total > done) {
      char remainStr[16];
      formatDuration((total - done) / speed, remainStr, sizeof remainStr);
      appendStr(line, sizeof line, &o, "~");
      appendStr(line, sizeof line, &o, remainStr);
      appendStr(line, sizeof line, &o, " left");
   } else {
      appendStr(line, sizeof line, &o, "estimating...");
   }
   line[o] = '\0';
   setLabelText(&statsBottom, line);
}

// busy mode: poll the owner's status/done each frame; circle asks the owner to stop.
static void updateBusy(void)
{
   if (busyDoneFn && busyDoneFn()) {
      ProgressDoneCallback cb = onDoneCb;
      int cancelled = cancelling;
      onDoneCb = NULL;
      busy = 0;
      hideOverlay(&progressOverlay);
      if (cb) cb(cancelled);
      return;
   }
   if (busyStatusFn) setLabelText(&subtitleLabel, strOrEmpty(busyStatusFn()));
   if (!cancelling && isPadButtonPressed(PAD_BTN_CIRCLE)) {
      playAudioOnce(clickSfx);
      cancelling = 1;
      if (busyCancelFn) busyCancelFn();
   }
}

static void update(void)
{
   if (busy) { updateBusy(); return; }

   // task finished: tear down and hand back to the caller on the main thread.
   if (!isTaskRunning()) {
      int cancelled = isCancelRequested();
      ProgressDoneCallback cb = onDoneCb;
      onDoneCb = NULL;
      hideOverlay(&progressOverlay);
      cancelling = 0;
      if (cb) cb(cancelled);
      return;
   }

   // live metrics: percentage every frame (cheap), stats lines throttled to once a second.
   uint64_t total = getTotalBytes();
   uint64_t done  = getProcessedBytes();
   // the total can be a lower bound (a size the source could only estimate), so done may pass it -
   // clamp, or the bar draws past the end of its track and out of the dialog.
   percent = total > 0 ? (int)((done * 100) / total) : 0;
   if (percent > 100) percent = 100;

   char pctStr[8];
   int o = appendUint64(pctStr, sizeof pctStr, 0, (uint64_t)percent);
   pctStr[o++] = '%';
   pctStr[o] = '\0';
   setLabelText(&percentLabel, pctStr);

   uint64_t elapsedSec = (sys_time_get_system_time() - startUs) / 1000000ULL;
   if (elapsedSec != lastStatsSec) {
      lastStatsSec = elapsedSec;
      updateStats(done, total, elapsedSec);
   }

   if (!cancelling && isPadButtonPressed(PAD_BTN_CIRCLE)) {
      playAudioOnce(clickSfx);
      cancelling = 1;
      cancelTask();
      setLabelText(&subtitleLabel, "Cancelling...");
   }
}

static void drawProgressBar(int x, int y, int width)
{
   int t = activeTheme->borderThickness;
   drawGfxBox(x, y, width, BAR_H, t, activeTheme->progressTrack, activeTheme->dialogBorder);
   int innerW = width - t * 2;
   int fillW = (int)((int64_t)innerW * percent / 100);
   if (fillW > 0) fillGfxRectangle(x + t, y + t, fillW, BAR_H - t * 2, activeTheme->progressFill);
}

#define BUSY_H 190   // shorter dialog for busy mode (no bar/stats)

// centered circle-glyph + label cancel hint along the bottom of a dialogX/dialogY/w/h box.
static void drawCancelHint(int dialogX, int dialogY, int w, int cancelY)
{
   int groupW = circleIcon.w + CANCEL_GAP + cancelLabel.tt.tex.w;
   int gx = dialogX + (w - groupW) / 2;
   drawImageAt(&circleIcon,  gx, dialogY + cancelY + (CANCEL_GLYPH - circleIcon.h) / 2);
   drawLabelAt(&cancelLabel, gx + circleIcon.w + CANCEL_GAP, dialogY + cancelY + (CANCEL_GLYPH - CANCEL_SIZE) / 2);
}

static void drawBusy(void)
{
   int dialogX = (getGfxScreenWidth()  - DIALOG_W) / 2;
   int dialogY = (getGfxScreenHeight() - BUSY_H)   / 2;

   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), activeTheme->scrim);
   drawGfxBox(dialogX, dialogY, DIALOG_W, BUSY_H, activeTheme->borderThickness, activeTheme->dialogFill, activeTheme->dialogBorder);

   drawLabelAt(&titleLabel,    dialogX + CONTENT_X, dialogY + TITLE_Y);
   drawLabelAt(&subtitleLabel, dialogX + CONTENT_X, dialogY + SUBTITLE_Y);
   drawCancelHint(dialogX, dialogY, DIALOG_W, BUSY_H - 48);
}

static void draw(void)
{
   if (busy) { drawBusy(); return; }

   int dialogX = (getGfxScreenWidth()  - DIALOG_W) / 2;
   int dialogY = (getGfxScreenHeight() - DIALOG_H) / 2;

   // scrim + flat panel
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), activeTheme->scrim);
   drawGfxBox(dialogX, dialogY, DIALOG_W, DIALOG_H, activeTheme->borderThickness, activeTheme->dialogFill, activeTheme->dialogBorder);

   drawLabelAt(&titleLabel,    dialogX + CONTENT_X, dialogY + TITLE_Y);
   drawLabelAt(&percentLabel,  dialogX + DIALOG_W - CONTENT_X - percentLabel.tt.tex.w, dialogY + PERCENT_Y);
   drawLabelAt(&subtitleLabel, dialogX + CONTENT_X, dialogY + SUBTITLE_Y);

   drawProgressBar(dialogX + CONTENT_X, dialogY + BAR_Y, DIALOG_W - CONTENT_X * 2);

   drawLabelAt(&statsTop,    dialogX + CONTENT_X, dialogY + STATS_Y);
   drawLabelAt(&statsBottom, dialogX + CONTENT_X, dialogY + STATS_Y + STATS_GAP);

   drawCancelHint(dialogX, dialogY, DIALOG_W, CANCEL_Y);
}

static void term(void)
{
   freeLabel(&titleLabel);
   freeLabel(&percentLabel);
   freeLabel(&subtitleLabel);
   freeLabel(&statsTop);
   freeLabel(&statsBottom);
   freeLabel(&cancelLabel);
   closeFont(&font);
   progressOverlay.status = OVERLAY_TERMINATED;
}

Overlay progressOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
