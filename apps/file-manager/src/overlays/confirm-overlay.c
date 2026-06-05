// confirm-overlay - centered modal yes/no dialog over a dimmed screen.
#include "overlays/confirm-overlay.h"
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
#define DIALOG_H       266
#define HIGHLIGHT_CAP  7    // highlight sprite (16x16) 9-slice corner cap
#define TEXT_RIGHT_PAD 20

// icons (dialog-relative)
#define WARNING_X      53
#define WARNING_Y      50
#define WARNING_W      75
#define WARNING_H      70
#define BUTTON_ICON    39   // native cross/square/circle glyph size

// button row: glyphs sit on one line, each followed by its label. the whole set
// is centered as a group (see draw), so only the row height and spacing are fixed.
#define BUTTON_ROW_Y   192  // y of the glyph row
#define ICON_LABEL_GAP 12   // space between a glyph and its own label
#define BUTTON_GAP     60   // space between one button's label and the next glyph
#define LABEL_DROP     10   // label y offset below the glyph top to center it

// texts (dialog-relative)
#define TITLE_X        168
#define TITLE_Y        50
#define TITLE_SIZE     24
#define MESSAGE_X      170
#define MESSAGE_Y      100
#define MESSAGE_SIZE   18
#define BUTTON_SIZE    18

// separator (dialog-relative)
#define SEP_X          45
#define SEP_Y          158
#define SEP_W          550
#define SEP_H          2

#define COLOR_SCRIM     0xC8000000  // black at 200/255
#define COLOR_DIALOG_BG 0xFF001636
#define COLOR_SUBTITLE  0x80FFFFFF

static Font            font;
static Audio          *clickSfx;
static ConfirmCallback onResolve;   // fired with the pressed button when one is chosen
static int             showSquare;  // 1 when the middle button is visible
static int             armed;       // 0 on the frame we open so the opening press is ignored

static NineSlice panel;
static Slice     separator;
static Image     warningIcon, crossIcon, squareIcon, circleIcon;
static Label     titleLabel, messageLabel, yesLabel, squareLabel, noLabel;

void initConfirmOverlay(GfxTexture sprites, Audio *sfx)
{
    clickSfx = sfx;
    font     = openSystemFont(FONT_POP);

    initNineSlice(&panel, sprites, 0, 0, DIALOG_W, DIALOG_H, spriteRegions[SPRITE_HIGHLIGHT], HIGHLIGHT_CAP, HIGHLIGHT_CAP);
    initSlice(&separator, sprites, 0, 0, SEP_W, SEP_H, spriteRegions[SPRITE_SEPARATOR], 1);

    initImage(&warningIcon, sprites, 0, 0, WARNING_W,   WARNING_H,   spriteRegions[SPRITE_WARNING], GFX_FILTER_LINEAR);
    initImage(&crossIcon,   sprites, 0, 0, BUTTON_ICON, BUTTON_ICON, spriteRegions[SPRITE_CROSS],   GFX_FILTER_LINEAR);
    initImage(&squareIcon,  sprites, 0, 0, BUTTON_ICON, BUTTON_ICON, spriteRegions[SPRITE_SQUARE],  GFX_FILTER_LINEAR);
    initImage(&circleIcon,  sprites, 0, 0, BUTTON_ICON, BUTTON_ICON, spriteRegions[SPRITE_CIRCLE],  GFX_FILTER_LINEAR);

    int textW = DIALOG_W - TITLE_X - TEXT_RIGHT_PAD;
    initLabel(&titleLabel,   &font, 0, 0, textW, AUTO, TITLE_SIZE,   COLOR_WHITE, TEXT_NOWRAP_ELLIPSIS, "");
    initLabel(&messageLabel, &font, 0, 0, textW, AUTO, MESSAGE_SIZE, COLOR_SUBTITLE, TEXT_NOWRAP_ELLIPSIS, "");
    initLabel(&yesLabel,     &font, 0, 0, 120,   AUTO, BUTTON_SIZE,  COLOR_WHITE, TEXT_NOWRAP,          "");
    initLabel(&squareLabel,  &font, 0, 0, 120,   AUTO, BUTTON_SIZE,  COLOR_WHITE, TEXT_NOWRAP,          "");
    initLabel(&noLabel,      &font, 0, 0, 120,   AUTO, BUTTON_SIZE,  COLOR_WHITE, TEXT_NOWRAP,          "");
}

void askConfirm(const char *title, const char *message,
                const char *crossText, const char *squareText, const char *circleText,
                ConfirmCallback onResult)
{
    onResolve  = onResult;
    showSquare = (squareText != NULL);
    setLabelText(&titleLabel,   strOrEmpty(title));
    setLabelText(&messageLabel, strOrEmpty(message));
    setLabelText(&yesLabel,     strOrEmpty(crossText));
    setLabelText(&squareLabel,  strOrEmpty(squareText));
    setLabelText(&noLabel,      strOrEmpty(circleText));
    showOverlay(&confirmOverlay);
}

static void show(void) { armed = 0; confirmOverlay.status = OVERLAY_VISIBLE; }
static void hide(void) { confirmOverlay.status = OVERLAY_HIDDEN; }

static void resolve(ConfirmChoice choice)
{
    ConfirmCallback cb = onResolve;
    onResolve = NULL;
    playSfxOnce(clickSfx);
    hideOverlay(&confirmOverlay);
    if (cb) cb(choice);
}

static void update(void)
{
    if (!armed) { armed = 1; return; }  // swallow the press that opened the dialog
    if (isButtonPressed(BTN_CROSS))                    resolve(CONFIRM_CROSS);
    else if (showSquare && isButtonPressed(BTN_SQUARE)) resolve(CONFIRM_SQUARE);
    else if (isButtonPressed(BTN_CIRCLE))              resolve(CONFIRM_CIRCLE);
}

static void draw(void)
{
    int sw = getGfxScreenWidth();
    int sh = getGfxScreenHeight();
    int dialogX = (sw - DIALOG_W) / 2;
    int dialogY = (sh - DIALOG_H) / 2;

    // dim the screen, then the dialog body and its rounded highlight border.
    // the border is drawn white-tinted, so the body colour comes from the fill.
    fillGfxRectangle(0, 0, sw, sh, COLOR_SCRIM);
    fillGfxRectangle(dialogX, dialogY, DIALOG_W, DIALOG_H, COLOR_DIALOG_BG);
    moveNineSlice(&panel, dialogX, dialogY);
    drawNineSlice(&panel);

    drawImageAt(&warningIcon,  dialogX + WARNING_X, dialogY + WARNING_Y);
    drawLabelAt(&titleLabel,   dialogX + TITLE_X,   dialogY + TITLE_Y);
    drawLabelAt(&messageLabel, dialogX + MESSAGE_X, dialogY + MESSAGE_Y);

    moveSlice(&separator, dialogX + SEP_X, dialogY + SEP_Y);
    drawSlice(&separator);

    // gather the visible buttons (cross, optional square, circle) and center them
    // as a group: each button is glyph + gap + label, BUTTON_GAP apart, with the
    // leftover width split evenly on both sides.
    Image *icons[3];
    Label *labels[3];
    int n = 0;
    icons[n] = &crossIcon;  labels[n] = &yesLabel;  n++;
    if (showSquare) { icons[n] = &squareIcon; labels[n] = &squareLabel; n++; }
    icons[n] = &circleIcon; labels[n] = &noLabel;  n++;

    int widths[3], total = 0;
    for (int i = 0; i < n; i++) {
        widths[i] = BUTTON_ICON + ICON_LABEL_GAP + (int)measureFontText(&font, BUTTON_SIZE, labels[i]->text);
        total += widths[i];
    }
    total += (n - 1) * BUTTON_GAP;

    int x = dialogX + (DIALOG_W - total) / 2;
    for (int i = 0; i < n; i++) {
        drawImageAt(icons[i],  x,                            dialogY + BUTTON_ROW_Y);
        drawLabelAt(labels[i], x + BUTTON_ICON + ICON_LABEL_GAP, dialogY + BUTTON_ROW_Y + LABEL_DROP);
        x += widths[i] + BUTTON_GAP;
    }
}

static void term(void)
{
    closeFont(&font);
    confirmOverlay.status = OVERLAY_TERMINATED;
}

Overlay confirmOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
