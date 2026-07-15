#pragma once

// scrollbar - flat thumb in a track, sized/positioned proportionally to how much of a scrollable
// list is currently visible. colours are passed in (the app supplies its theme's track/thumb).

#include "gfx.h"
#include <stdint.h>

typedef struct {
   int x, w;              // thumb/track column (centred in the track width at init)
   int trackY, trackH;
   int minThumbHeight;
   uint32_t trackColor, thumbColor;
} Scrollbar;

void initScrollbar(Scrollbar *sb, int trackX, int trackY, int trackW, int trackH,
                   int thumbWidth, int minThumbHeight, uint32_t trackColor, uint32_t thumbColor);

// recolour the track + thumb for a live theme switch.
void rethemeScrollbar(Scrollbar *sb, uint32_t trackColor, uint32_t thumbColor);

// draws the track plus a thumb sized for visibleItems out of totalItems, scrolled to scrollTop.
// no-ops once the whole list already fits on screen (totalItems <= visibleItems).
void drawScrollbar(Scrollbar *sb, int totalItems, int visibleItems, int scrollTop);
