// shortcut-hint - see shortcut-hint.h. a single centred horizontal row of the console's own button
// glyphs with captions, on a translucent bottom strip: one leading SELECT glyph (the shared
// modifier), then each action's button glyph and name.

#include "shortcut-hint.h"
#include "shortcuts.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "gfx.h"
#include "net-common.h"   // getTimeUs

#define HINT_HOLD_US   10000000
#define GLYPH_HEIGHT   30
#define CAPTION_SIZE   22
#define BAR_PADDING    16
#define BAR_BOTTOM_MARGIN 90         // clear of the bottom edge, matching the title's top margin
#define BAR_BACKGROUND 0xE6000000    // ~90% opaque black
#define CAPTION_COLOR  0xFFDDDDDD

#define HIDE_GLYPH_HEIGHT 20         // the small "[SELECT][stats] Hide" row under the stats graph
#define HIDE_CAPTION_SIZE 15

static ButtonHints bar;
static int barY, barHeight;
static uint64_t shownUs;
static int active;

static ButtonHints hideBar;   // the stats panel's own footer hint

static ConsoleGlyph glyphForButton(PadButton button)
{
   switch (button) {
   case PAD_BTN_CROSS:    return GLYPH_CROSS;
   case PAD_BTN_CIRCLE:   return GLYPH_CIRCLE;
   case PAD_BTN_SQUARE:   return GLYPH_SQUARE;
   case PAD_BTN_TRIANGLE: return GLYPH_TRIANGLE;
   case PAD_BTN_L1:       return GLYPH_L1;
   case PAD_BTN_R1:       return GLYPH_R1;
   case PAD_BTN_L2:       return GLYPH_L2;
   case PAD_BTN_R2:       return GLYPH_R2;
   case PAD_BTN_R3:       return GLYPH_R3;
   case PAD_BTN_START:    return GLYPH_START;
   case PAD_BTN_SELECT:   return GLYPH_SELECT;
   case PAD_BTN_UP:       return GLYPH_DPAD_UP;
   case PAD_BTN_DOWN:     return GLYPH_DPAD_DOWN;
   case PAD_BTN_LEFT:     return GLYPH_DPAD_LEFT;
   case PAD_BTN_RIGHT:    return GLYPH_DPAD_RIGHT;
   default:               return GLYPH_COUNT;   // e.g. L3 has no glyph
   }
}

void initShortcutHint(Font *font)
{
   loadConsoleGlyphs();   // idempotent; captions still show if the font is unreadable

   barHeight = GLYPH_HEIGHT + BAR_PADDING * 2;
   barY = getGfxScreenHeight() - barHeight - BAR_BOTTOM_MARGIN;
   initButtonHints(&bar, font, barY + BAR_PADDING, GLYPH_HEIGHT, CAPTION_SIZE, CAPTION_COLOR);

   // each entry shows the SELECT modifier next to its button: [SELECT][button] name. the SELECT
   // glyph is caption-less so it clusters tightly with the button that follows it.
   for (int action = 0; action < SHORTCUT_COUNT; action++) {
      ConsoleGlyph glyph = glyphForButton(getShortcutButton((ShortcutAction)action));
      if (glyph == GLYPH_COUNT) continue;
      addButtonHint(&bar, getConsoleGlyph(GLYPH_SELECT), "");
      addButtonHint(&bar, getConsoleGlyph(glyph), getShortcutActionName((ShortcutAction)action));
   }

   // the stats panel's footer: [SELECT][stats-button] Hide. if the bound button has no glyph
   // (e.g. L3), caption the SELECT glyph itself so "Hide" still reads.
   initButtonHints(&hideBar, font, 0, HIDE_GLYPH_HEIGHT, HIDE_CAPTION_SIZE, CAPTION_COLOR);
   ConsoleGlyph statsGlyph = glyphForButton(getShortcutButton(SHORTCUT_STATS));
   if (statsGlyph != GLYPH_COUNT) {
      addButtonHint(&hideBar, getConsoleGlyph(GLYPH_SELECT), "");
      addButtonHint(&hideBar, getConsoleGlyph(statsGlyph), "Hide");
   } else {
      addButtonHint(&hideBar, getConsoleGlyph(GLYPH_SELECT), "Hide");
   }
}

int getStatsHideHintHeight(void) { return HIDE_GLYPH_HEIGHT; }

void drawStatsHideHint(int centerWidth, int y)
{
   hideBar.y = y;
   drawButtonHints(&hideBar, centerWidth);   // centred within [0, centerWidth]
}

void showShortcutHint(void)
{
   shownUs = getTimeUs();
   active = 1;
}

void updateShortcutHint(void)
{
   if (active && getTimeUs() - shownUs >= HINT_HOLD_US) active = 0;
}

int isShortcutHintActive(void) { return active; }

void drawShortcutHint(void)
{
   fillGfxRectangle(0, barY, getGfxScreenWidth(), barHeight, BAR_BACKGROUND);
   drawButtonHints(&bar, getGfxScreenWidth());
}

void freeShortcutHint(void)
{
   termButtonHints(&bar);
   termButtonHints(&hideBar);
}
