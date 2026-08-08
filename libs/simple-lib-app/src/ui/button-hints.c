// button-hints - centered "[glyph] caption" pad-hint row (see ui/button-hints.h).

#include "ui/button-hints.h"
#include <string.h>

#define ICON_GAP 8    // between a glyph and its caption
#define HINT_GAP 30   // between one hint and the next
#define PAIR_GAP 8    // between two caption-less glyphs that form a pair (L1 + R1)

void initButtonHints(ButtonHints *bar, Font *font, int y, int glyphHeight, int captionSize, uint32_t captionColor)
{
   memset(bar, 0, sizeof *bar);
   bar->font         = font;
   bar->y            = y;
   bar->glyphHeight  = glyphHeight;
   bar->captionSize  = captionSize;
   bar->captionColor = captionColor;
}

int addButtonHint(ButtonHints *bar, GfxTexture glyph, const char *caption)
{
   if (bar->count >= MAX_BUTTON_HINTS) return -1;
   ButtonHint *hint = &bar->hints[bar->count++];
   hint->glyph     = glyph;
   hint->drawWidth = glyph.h > 0 ? glyph.w * bar->glyphHeight / glyph.h : bar->glyphHeight;
   hint->pairNext  = (caption == NULL || caption[0] == 0);
   initLabel(&hint->caption, bar->font, 0, 0, AUTO, AUTO, bar->captionSize, bar->captionColor, TEXT_NOWRAP, caption);
   // a caption-less hint is just the glyph; drop the icon gap so it clusters with the next one
   hint->width = hint->drawWidth + (hint->pairNext ? 0 : ICON_GAP + hint->caption.tt.tex.w);
   return bar->count - 1;
}

void setButtonHintCaption(ButtonHints *bar, int index, const char *caption)
{
   if (index < 0 || index >= bar->count) return;
   ButtonHint *hint = &bar->hints[index];
   setLabelText(&hint->caption, caption);
   hint->pairNext = (caption == NULL || caption[0] == 0);
   hint->width = hint->drawWidth + (hint->pairNext ? 0 : ICON_GAP + hint->caption.tt.tex.w);
}

static int drawHint(ButtonHints *bar, ButtonHint *hint, int x)
{
   int rowMid = bar->y + bar->glyphHeight / 2;
   if (hint->glyph.offset)
      drawGfxTexture(x, bar->y, hint->drawWidth, bar->glyphHeight, hint->glyph, 0, 0, 1, 1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
   x += hint->drawWidth;
   if (hint->pairNext) return x;   // glyph only - the caption belongs to the hint it pairs with
   x += ICON_GAP;
   moveLabel(&hint->caption, x, rowMid - hint->caption.tt.tex.h / 2);
   drawLabel(&hint->caption);
   return x + hint->caption.tt.tex.w;
}

void setButtonHintGap(ButtonHints *bar, int gap)
{
   bar->gap = gap;
}

void setButtonHintShown(ButtonHints *bar, int index, int shown)
{
   if (index < 0 || index >= bar->count) return;
   bar->hints[index].hidden = !shown;
}

void drawButtonHintsAt(ButtonHints *bar, int x)
{
   for (int i = 0; i < bar->count; i++) {
      if (bar->hints[i].hidden) continue;

      x = drawHint(bar, &bar->hints[i], x);
      x += bar->hints[i].pairNext ? PAIR_GAP : (bar->gap > 0 ? bar->gap : HINT_GAP);
   }
}

void drawButtonHints(ButtonHints *bar, int screenWidth)
{
   int total = 0;
   for (int i = 0; i < bar->count; i++) {
      if (bar->hints[i].hidden) continue;

      total += bar->hints[i].width;
      if (i < bar->count - 1) total += bar->hints[i].pairNext ? PAIR_GAP : (bar->gap > 0 ? bar->gap : HINT_GAP);
   }

   drawButtonHintsAt(bar, (screenWidth - total) / 2);
}

void termButtonHints(ButtonHints *bar)
{
   for (int i = 0; i < bar->count; i++) freeLabel(&bar->hints[i].caption);
   bar->count = 0;
}
