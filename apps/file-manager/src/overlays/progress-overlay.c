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
// flush against the frame edge.
#define FRAME_X        45
#define FRAME_Y        150
#define FRAME_W        487
#define FRAME_H        24
#define FRAME_CAP      3    // frame sprite is 8px; cap<4 leaves a stretchable middle
#define BAR_PAD        5
#define PROGRESS_CAP   4    // progress sprite is 10px
#define BAR_MIN_W      (2 * PROGRESS_CAP)  // below this a 9-slice can't render cleanly

// percentage label, to the right of the bar
#define PCT_X          550
#define PCT_W          (DIALOG_W - PCT_X - TEXT_RIGHT_PAD)
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

#define COLOR_SCRIM     0xC8000000  // black at 200/255
#define COLOR_DIALOG_BG 0xFF001636
#define COLOR_SUBTITLE  0x80FFFFFF

static Font       font;
static Audio     *clickSfx;
static GfxTexture sprites;

static NineSlice panel;
static NineSlice frame;    // border around the bar
static NineSlice barFill;  // the progress fill
static Slice     separator;
static Image     circleIcon;
static Label     titleLabel, subtitleLabel, pctLabel, cancelLabel;

static ProgressDoneCallback onDoneCb;
static int      cancelling;  // cancel requested, awaiting the worker to exit

void initProgressOverlay(GfxTexture s, Audio *sfx)
{
    sprites  = s;
    clickSfx = sfx;
    font     = openSystemFont(FONT_POP);

    initNineSlice(&panel,   sprites, 0, 0, DIALOG_W, DIALOG_H, spriteRegions[SPRITE_HIGHLIGHT], HIGHLIGHT_CAP, HIGHLIGHT_CAP);
    initNineSlice(&frame,   sprites, 0, 0, FRAME_W, FRAME_H, spriteRegions[SPRITE_FRAME], FRAME_CAP, FRAME_CAP);
    initNineSlice(&barFill, sprites, 0, 0, BAR_MIN_W, FRAME_H - 2 * BAR_PAD, spriteRegions[SPRITE_PROGRESS], PROGRESS_CAP, PROGRESS_CAP);
    initSlice(&separator, sprites, 0, 0, SEP_W, SEP_H, spriteRegions[SPRITE_SEPARATOR], 1);
    initImage(&circleIcon, sprites, 0, 0, CANCEL_ICON, CANCEL_ICON, spriteRegions[SPRITE_CIRCLE], GFX_FILTER_LINEAR);

    initLabel(&titleLabel,    &font, 0, 0, DIALOG_W - TITLE_X - TEXT_RIGHT_PAD,    AUTO, TITLE_SIZE,    COLOR_WHITE,    TEXT_NOWRAP_ELLIPSIS, "");
    initLabel(&subtitleLabel, &font, 0, 0, DIALOG_W - SUBTITLE_X - TEXT_RIGHT_PAD, AUTO, SUBTITLE_SIZE, COLOR_SUBTITLE, TEXT_NOWRAP_ELLIPSIS, "");
    initLabel(&pctLabel,      &font, 0, 0, PCT_W,  AUTO, PCT_SIZE,    COLOR_WHITE, TEXT_NOWRAP, "");
    initLabel(&cancelLabel,   &font, 0, 0, 120,    AUTO, CANCEL_SIZE, COLOR_WHITE, TEXT_NOWRAP, "Cancel");
}

void startProgress(const char *title, const char *subtitle, TaskBody run, ProgressDoneCallback onDone)
{
    onDoneCb   = onDone;
    cancelling = 0;
    setLabelText(&titleLabel,    strOrEmpty(title));
    setLabelText(&subtitleLabel, strOrEmpty(subtitle));
    setLabelText(&pctLabel,      "0%");

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
    int sw = getGfxScreenWidth();
    int sh = getGfxScreenHeight();
    int dialogX = (sw - DIALOG_W) / 2;
    int dialogY = (sh - DIALOG_H) / 2;

    fillGfxRectangle(0, 0, sw, sh, COLOR_SCRIM);
    fillGfxRectangle(dialogX, dialogY, DIALOG_W, DIALOG_H, COLOR_DIALOG_BG);
    moveNineSlice(&panel, dialogX, dialogY);
    drawNineSlice(&panel);

    drawLabelAt(&titleLabel,    dialogX + TITLE_X,    dialogY + TITLE_Y);
    drawLabelAt(&subtitleLabel, dialogX + SUBTITLE_X, dialogY + SUBTITLE_Y);

    // frame around the bar
    moveNineSlice(&frame, dialogX + FRAME_X, dialogY + FRAME_Y);
    drawNineSlice(&frame);

    // fill, inset by BAR_PAD so it isn't flush with the frame
    uint64_t total = getTotalBytes();
    uint64_t done  = getProcessedBytes();
    int pct = total > 0 ? (int)((done * 100) / total) : 0;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    int barMaxW = FRAME_W - 2 * BAR_PAD;
    int fillW   = barMaxW * pct / 100;
    if (fillW > 0) {
        if (fillW < BAR_MIN_W) fillW = BAR_MIN_W;  // keep the 9-slice legible
        if (fillW > barMaxW)   fillW = barMaxW;
        barFill.w = fillW;
        moveNineSlice(&barFill, dialogX + FRAME_X + BAR_PAD, dialogY + FRAME_Y + BAR_PAD);
        drawNineSlice(&barFill);
    }

    // percentage to the right of the bar, vertically centered to the frame
    char pctStr[8];
    int o = intToDec(pct, pctStr);
    pctStr[o++] = '%';
    pctStr[o]   = '\0';
    setLabelText(&pctLabel, pctStr);
    drawLabelAt(&pctLabel, dialogX + PCT_X, dialogY + FRAME_Y + (FRAME_H - PCT_SIZE) / 2);

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
    freeLabel(&pctLabel);
    freeLabel(&cancelLabel);
    closeFont(&font);
    progressOverlay.status = OVERLAY_TERMINATED;
}

Overlay progressOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
