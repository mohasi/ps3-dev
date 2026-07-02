// progress-bar - framed 9-slice fill + percentage label
#include "ui/progress-bar.h"
#include "string-utilities.h"

void initProgressBar(ProgressBar *bar, GfxTexture sprites, Font *font, int frameW, int frameH,
                      SpriteRegion frameSprite, int frameCap, SpriteRegion fillSprite, int fillCap, int fillPad,
                      int pctLabelWidth, int pctSize, uint32_t pctColor, int pctGap)
{
   bar->fillPad      = fillPad;
   bar->minFillWidth = 2 * fillCap;   // below this a 9-slice fill can't render cleanly
   bar->pctGap       = pctGap;

   initNineSlice(&bar->frame, sprites, 0, 0, frameW, frameH, frameSprite, frameCap, frameCap);
   initNineSlice(&bar->fill, sprites, 0, 0, bar->minFillWidth, frameH - 2 * fillPad, fillSprite, fillCap, fillCap);
   initLabel(&bar->pct, font, 0, 0, pctLabelWidth, AUTO, pctSize, pctColor, TEXT_NOWRAP, "");
}

void drawProgressBarAt(ProgressBar *bar, int x, int y, int percent)
{
   if (percent < 0)   percent = 0;
   if (percent > 100) percent = 100;

   moveNineSlice(&bar->frame, x, y);
   drawNineSlice(&bar->frame);

   int barMaxW = bar->frame.w - 2 * bar->fillPad;
   int fillW   = barMaxW * percent / 100;
   if (fillW > 0) {
      if (fillW < bar->minFillWidth) fillW = bar->minFillWidth;
      if (fillW > barMaxW)           fillW = barMaxW;
      bar->fill.w = fillW;
      moveNineSlice(&bar->fill, x + bar->fillPad, y + bar->fillPad);
      drawNineSlice(&bar->fill);
   }

   char pctText[8];
   int o = intToDec(percent, pctText);
   pctText[o++] = '%';
   pctText[o]   = '\0';
   setLabelText(&bar->pct, pctText);
   drawLabelAt(&bar->pct, x + bar->frame.w + bar->pctGap, y + (bar->frame.h - bar->pct.size) / 2);
}

void freeProgressBar(ProgressBar *bar)
{
   freeLabel(&bar->pct);
}
