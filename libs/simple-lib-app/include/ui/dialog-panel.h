#pragma once

// dialog-panel - dims the screen, then draws a centered, bordered modal body.
// x/y are the panel's last-drawn top-left corner, so callers can position
// their own content (icons/labels/buttons) relative to it.

#include "ui/slice.h"

typedef struct {
   NineSlice border;
   int w, h;
   uint32_t bgColor;
   int x, y;
} DialogPanel;

void initDialogPanel(DialogPanel *panel, GfxTexture sprites, int w, int h, uint32_t bgColor,
                      SpriteRegion borderSprite, int cap);

// centers the panel on the current screen size, dims the background, and draws
// the panel body + border. updates panel->x/y for the caller to read.
void drawDialogPanel(DialogPanel *panel);
