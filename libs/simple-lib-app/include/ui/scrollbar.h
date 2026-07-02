#pragma once

// scrollbar - pill-shaped thumb in a track, sized/positioned proportionally to
// how much of a scrollable list is currently visible.

#include "ui/slice.h"

typedef struct {
   NineSlice thumb;
   int trackY, trackH;
   int minThumbHeight;
} Scrollbar;

void initScrollbar(Scrollbar *sb, GfxTexture sprites, int trackX, int trackY, int trackW, int trackH,
                    SpriteRegion thumbSprite, int thumbWidth, int cap, int minThumbHeight);

// draws the thumb sized for visibleItems out of totalItems, scrolled to scrollTop.
// no-ops once the whole list already fits on screen (totalItems <= visibleItems).
void drawScrollbar(Scrollbar *sb, int totalItems, int visibleItems, int scrollTop);
