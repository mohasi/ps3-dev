#pragma once

// progress-bar - framed track with a proportional fill and a "N%" label to its right.

#include "ui/slice.h"
#include "ui/label.h"

typedef struct {
   NineSlice frame;
   NineSlice fill;
   Label     pct;
   int       fillPad;
   int       minFillWidth;
   int       pctGap;
} ProgressBar;

void initProgressBar(ProgressBar *bar, GfxTexture sprites, Font *font, int frameW, int frameH,
                      SpriteRegion frameSprite, int frameCap, SpriteRegion fillSprite, int fillCap, int fillPad,
                      int pctLabelWidth, int pctSize, uint32_t pctColor, int pctGap);

// draws the frame, the fill sized to percent (clamped 0-100), and the percentage label.
void drawProgressBarAt(ProgressBar *bar, int x, int y, int percent);

void freeProgressBar(ProgressBar *bar);
