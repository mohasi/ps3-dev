// progress-overlay - centered modal with a framed progress bar and a Cancel
// button, driving a background file task (see file-task.h).
#include "overlays/progress-overlay.h"
#include "file-task.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "audio.h"
#include "colors.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/slice.h"
#include "ui/dialog-panel.h"
#include "ui/progress-bar.h"
#include "string-utilities.h"
#include "sprite-regions.h"

#define DIALOG_W       640
#define DIALOG_H       280
#define HIGHLIGHT_CAP  7    // highlight sprite (16x16) 9-slice corner cap
#define TEXT_RIGHT_PAD 20

// title / subtitle (dialog-relative)
#define TITLE_X        45
#define TITLE_Y        50
#define TITLE_SIZE     24
#define SUBTITLE_X     45
#define SUBTITLE_Y     100
#define SUBTITLE_SIZE  18

// progress bar (dialog-relative). FRAME is the 8x8 9-slice border; the fill is
// the 10x10 PROGRESS 9-slice, inset by BAR_PAD on every side so it never sits
// flush against the frame edge. the percentage label sits PCT_GAP past the frame.
#define FRAME_X        45
#define FRAME_Y        150
#define FRAME_W        487
#define FRAME_H        24
#define FRAME_CAP      3    // frame sprite is 8px; cap<4 leaves a stretchable middle
#define BAR_PAD        5
#define PROGRESS_CAP   4    // progress sprite is 10px
#define PCT_GAP        18
#define PCT_W          (DIALOG_W - FRAME_X - FRAME_W - PCT_GAP - TEXT_RIGHT_PAD)
#define PCT_SIZE       20

// separator (dialog-relative)
#define SEP_X          45
#define SEP_Y          196
#define SEP_W          550
#define SEP_H          2

// cancel button (circle icon + label, centered as a group)
#define CANCEL_ICON    39   // native circle glyph size
#define CANCEL_Y       215
#define CANCEL_GAP     10
#define CANCEL_SIZE    18

#define COLOR_DIALOG_BG 0xFF001636
#define COLOR_SUBTITLE  0x80FFFFFF

static Font font;
static Audio *clickSfx;

static DialogPanel panel;
static ProgressBar bar;
static Slice     separator;
static Image     circleIcon;
static Label     titleLabel, subtitleLabel, cancelLabel;

static ProgressDoneCallback onDoneCb;
static int      cancelling;  // cancel requested, awaiting the worker to exit

void initProgressOverlay(GfxTexture sprites, Audio *sfx)
{
   clickSfx = sfx;
   font     = openSystemFont(FONT_POP);

   initDialogPanel(&panel, sprites, DIALOG_W, DIALOG_H, COLOR_DIALOG_BG, spriteRegions[SPRITE_HIGHLIGHT], HIGHLIGHT_CAP);
   initProgressBar(&bar, sprites, &font, FRAME_W, FRAME_H, spriteRegions[SPRITE_FRAME], FRAME_CAP,
                   spriteRegions[SPRITE_PROGRESS], PROGRESS_CAP, BAR_PAD, PCT_W, PCT_SIZE, COLOR_WHITE, PCT_GAP);
   initSlice(&separator, sprites, 0, 0, SEP_W, SEP_H, spriteRegions[SPRITE_SEPARATOR], 1);
   initImage(&circleIcon, sprites, 0, 0, CANCEL_ICON, CANCEL_ICON, spriteRegions[SPRITE_CIRCLE], GFX_FILTER_LINEAR);

   initLabel(&titleLabel,    &font, 0, 0, DIALOG_W - TITLE_X - TEXT_RIGHT_PAD,    AUTO, TITLE_SIZE,    COLOR_WHITE,    TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&subtitleLabel, &font, 0, 0, DIALOG_W - SUBTITLE_X - TEXT_RIGHT_PAD, AUTO, SUBTITLE_SIZE, COLOR_SUBTITLE, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&cancelLabel,   &font, 0, 0, 120, AUTO, CANCEL_SIZE, COLOR_WHITE, TEXT_NOWRAP, "Cancel");
}

void startProgress(const char *title, const char *subtitle, TaskBody run, ProgressDoneCallback onDone)
{
   onDoneCb   = onDone;
   cancelling = 0;
   setLabelText(&titleLabel,    strOrEmpty(title));
   setLabelText(&subtitleLabel, strOrEmpty(subtitle));

   startTask(run);
   showOverlay(&progressOverlay);  // dialog shows from 0% while the task runs
}

static void show(void) { progressOverlay.status = OVERLAY_VISIBLE; }
static void hide(void) { progressOverlay.status = OVERLAY_HIDDEN; }

static void update(void)
{
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

   if (!cancelling && isPadButtonPressed(PAD_BTN_CIRCLE)) {
      playSfxOnce(clickSfx);
      cancelling = 1;
      cancelTask();
      setLabelText(&subtitleLabel, "Cancelling...");
   }
}

static void draw(void)
{
   drawDialogPanel(&panel);
   int dialogX = panel.x, dialogY = panel.y;

   drawLabelAt(&titleLabel,    dialogX + TITLE_X,    dialogY + TITLE_Y);
   drawLabelAt(&subtitleLabel, dialogX + SUBTITLE_X, dialogY + SUBTITLE_Y);

   uint64_t total = getTotalBytes();
   uint64_t done  = getProcessedBytes();
   int pct = total > 0 ? (int)((done * 100) / total) : 0;
   drawProgressBarAt(&bar, dialogX + FRAME_X, dialogY + FRAME_Y, pct);

   moveSlice(&separator, dialogX + SEP_X, dialogY + SEP_Y);
   drawSlice(&separator);

   // single Cancel button: circle icon + label, centered as a group
   float tw = measureFontText(&font, CANCEL_SIZE, "Cancel");
   int groupW = CANCEL_ICON + CANCEL_GAP + (int)tw;
   int gx = dialogX + (DIALOG_W - groupW) / 2;
   drawImageAt(&circleIcon,  gx, dialogY + CANCEL_Y);
   drawLabelAt(&cancelLabel, gx + CANCEL_ICON + CANCEL_GAP, dialogY + CANCEL_Y + (CANCEL_ICON - CANCEL_SIZE) / 2);
}

static void term(void)
{
   freeLabel(&titleLabel);
   freeLabel(&subtitleLabel);
   freeProgressBar(&bar);
   freeLabel(&cancelLabel);
   closeFont(&font);
   progressOverlay.status = OVERLAY_TERMINATED;
}

Overlay progressOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
