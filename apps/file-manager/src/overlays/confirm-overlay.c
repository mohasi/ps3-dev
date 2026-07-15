// confirm-overlay - centered modal yes/no dialog over a dimmed screen, flat/metro style.
// each choice is a face button (cross / optional square / circle): a flat box with the pad glyph and
// its label. all buttons look the same - you press the matching pad button, there is no focus to move.
// the panel sizes itself to the content: width fits the widest of title / message / button row (a long
// message wraps at MAX_CONTENT), height stacks title + message + buttons. all colours come from the
// active theme, so it follows a theme switch (see theme.h).
#include "overlays/confirm-overlay.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "audio.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/console-glyphs.h"
#include "string-utilities.h"
#include "theme.h"

// content sizing (the text block, before the CONTENT_X padding on each side)
#define CONTENT_X      54   // left/right padding between the panel edge and content
#define MIN_CONTENT    320  // keep short prompts from looking cramped
#define MAX_CONTENT    760  // past this the message wraps instead of widening the panel

// vertical stack (dialog-relative), all measured from the real rendered text heights
#define TOP_PAD        44   // title top
#define TITLE_MSG_GAP  16
#define MSG_BUTTON_GAP 30
#define BOTTOM_PAD     32
#define TITLE_SIZE     26
#define MESSAGE_SIZE   18

// button bar: just glyph + label per choice (no boxes - they aren't clickable/focusable, you press the
// matching pad button), centered as a group along the bottom
#define BUTTON_GLYPH    28   // rendered height of the pad glyph
#define BUTTON_SIZE     18   // button label text size
#define BUTTON_ROW_H    32   // height of the glyph/label band, for vertical centering
#define GLYPH_LABEL_GAP 10   // space between a glyph and its label
#define BUTTON_GAP      44   // space between one button's label and the next glyph

static Font            font;
static Audio          *clickSfx;
static ConfirmCallback onResolve;   // fired with the pressed button when one is chosen
static int             showSquare;  // 1 when the middle button is visible
static int             showCircle;  // 1 when the circle button is visible (0 = single-OK alert)
static int             armed;       // 0 on the frame we open so the opening press is ignored

static Image crossIcon, squareIcon, circleIcon;
static Label titleLabel, messageLabel, yesLabel, squareLabel, noLabel;

// content-fitted geometry, recomputed by layout() each time the dialog opens
static int   dialogW, dialogH, messageY, buttonRowY;
static int   buttonCount, buttonWidths[3], buttonRowWidth;
static Image *buttonIcons[3];
static Label *buttonLabels[3];

void initConfirmOverlay(Audio *sfx)
{
   clickSfx = sfx;
   font     = openSystemFont(FONT_POP);

   initGlyphIcon(&crossIcon,  GLYPH_CROSS,  BUTTON_GLYPH);
   initGlyphIcon(&squareIcon, GLYPH_SQUARE, BUTTON_GLYPH);
   initGlyphIcon(&circleIcon, GLYPH_CIRCLE, BUTTON_GLYPH);

   // width params are the wrap/measure bounds; the panel then shrinks to the actual rendered width.
   initLabel(&titleLabel,   &font, 0, 0, MAX_CONTENT, AUTO, TITLE_SIZE,   activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&messageLabel, &font, 0, 0, MAX_CONTENT, AUTO, MESSAGE_SIZE, activeTheme->textSecondary, TEXT_WRAP,            "");
   initLabel(&yesLabel,     &font, 0, 0, MAX_CONTENT, AUTO, BUTTON_SIZE,  activeTheme->textPrimary,   TEXT_NOWRAP,          "");
   initLabel(&squareLabel,  &font, 0, 0, MAX_CONTENT, AUTO, BUTTON_SIZE,  activeTheme->textPrimary,   TEXT_NOWRAP,          "");
   initLabel(&noLabel,      &font, 0, 0, MAX_CONTENT, AUTO, BUTTON_SIZE,  activeTheme->textPrimary,   TEXT_NOWRAP,          "");
}

// fit the panel to the current text: collect the visible buttons, then size width to the widest row
// and height to the stacked title/message/buttons.
static void layout(void)
{
   buttonCount = 0;
   buttonIcons[buttonCount] = &crossIcon;  buttonLabels[buttonCount] = &yesLabel;    buttonCount++;
   if (showSquare) { buttonIcons[buttonCount] = &squareIcon; buttonLabels[buttonCount] = &squareLabel; buttonCount++; }
   if (showCircle) { buttonIcons[buttonCount] = &circleIcon; buttonLabels[buttonCount] = &noLabel;     buttonCount++; }

   buttonRowWidth = 0;
   for (int i = 0; i < buttonCount; i++) {
      buttonWidths[i] = buttonIcons[i]->w + GLYPH_LABEL_GAP + buttonLabels[i]->tt.tex.w;
      buttonRowWidth += buttonWidths[i];
   }
   buttonRowWidth += (buttonCount - 1) * BUTTON_GAP;

   int contentW = titleLabel.tt.tex.w;
   if (messageLabel.tt.tex.w > contentW) contentW = messageLabel.tt.tex.w;
   if (buttonRowWidth        > contentW) contentW = buttonRowWidth;
   if (contentW < MIN_CONTENT) contentW = MIN_CONTENT;
   if (contentW > MAX_CONTENT) contentW = MAX_CONTENT;
   dialogW = contentW + CONTENT_X * 2;

   messageY   = TOP_PAD + titleLabel.tt.tex.h + TITLE_MSG_GAP;
   buttonRowY = messageY + messageLabel.tt.tex.h + MSG_BUTTON_GAP;
   dialogH    = buttonRowY + BUTTON_ROW_H + BOTTOM_PAD;
}

void askConfirm(const char *title, const char *message,
                const char *crossText, const char *squareText, const char *circleText,
                ConfirmCallback onResult)
{
   onResolve  = onResult;
   showSquare = (squareText != NULL);
   showCircle = (circleText != NULL);
   setLabelText(&titleLabel,   strOrEmpty(title));
   setLabelText(&messageLabel, strOrEmpty(message));
   setLabelText(&yesLabel,     strOrEmpty(crossText));
   setLabelText(&squareLabel,  strOrEmpty(squareText));
   setLabelText(&noLabel,      strOrEmpty(circleText));
   layout();
   showOverlay(&confirmOverlay);
}

// labels capture their colour at init, so a live theme switch needs this (the scrim/panel read the
// theme live and follow for free). runs while the dialog is hidden; the next open renders in the new
// colour. see applyThemeToHome.
void rethemeConfirmOverlay(void)
{
   setLabelColor(&titleLabel,   activeTheme->textPrimary);
   setLabelColor(&messageLabel, activeTheme->textSecondary);
   setLabelColor(&yesLabel,     activeTheme->textPrimary);
   setLabelColor(&squareLabel,  activeTheme->textPrimary);
   setLabelColor(&noLabel,      activeTheme->textPrimary);
}

static void show(void) { armed = 0; confirmOverlay.status = OVERLAY_VISIBLE; }
static void hide(void) { confirmOverlay.status = OVERLAY_HIDDEN; }

static void resolve(ConfirmChoice choice)
{
   ConfirmCallback cb = onResolve;
   onResolve = NULL;
   playAudioOnce(clickSfx);
   hideOverlay(&confirmOverlay);
   if (cb) cb(choice);
}

static void update(void)
{
   if (!armed) { armed = 1; return; }  // swallow the press that opened the dialog
   if (isPadButtonPressed(PAD_BTN_CROSS))                       resolve(CONFIRM_CROSS);
   else if (showSquare && isPadButtonPressed(PAD_BTN_SQUARE))   resolve(CONFIRM_SQUARE);
   else if (showCircle && isPadButtonPressed(PAD_BTN_CIRCLE))   resolve(CONFIRM_CIRCLE);
}

// one choice: pad glyph + its label, vertically centered in the button band. no box - you just press
// the matching face button, there is nothing to click or focus.
static void drawButton(int x, int rowY, Image *glyph, Label *label)
{
   drawImageAt(glyph, x, rowY + (BUTTON_ROW_H - glyph->h) / 2);
   drawLabelAt(label, x + glyph->w + GLYPH_LABEL_GAP, rowY + (BUTTON_ROW_H - BUTTON_SIZE) / 2 - 2);
}

static void draw(void)
{
   int dialogX = (getGfxScreenWidth()  - dialogW) / 2;
   int dialogY = (getGfxScreenHeight() - dialogH) / 2;

   // scrim + flat panel
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), activeTheme->scrim);
   drawGfxBox(dialogX, dialogY, dialogW, dialogH, activeTheme->borderThickness, activeTheme->dialogFill, activeTheme->dialogBorder);

   drawLabelAt(&titleLabel,   dialogX + CONTENT_X, dialogY + TOP_PAD);
   drawLabelAt(&messageLabel, dialogX + CONTENT_X, dialogY + messageY);

   // button row centered as a group along the bottom
   int x = dialogX + (dialogW - buttonRowWidth) / 2;
   int rowY = dialogY + buttonRowY;
   for (int i = 0; i < buttonCount; i++) {
      drawButton(x, rowY, buttonIcons[i], buttonLabels[i]);
      x += buttonWidths[i] + BUTTON_GAP;
   }
}

static void term(void)
{
   freeLabel(&titleLabel);
   freeLabel(&messageLabel);
   freeLabel(&yesLabel);
   freeLabel(&squareLabel);
   freeLabel(&noLabel);
   closeFont(&font);
   confirmOverlay.status = OVERLAY_TERMINATED;
}

Overlay confirmOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
