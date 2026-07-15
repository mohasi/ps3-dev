#pragma once

// icon-font - scalable, theme-tintable UI icons from the embedded icon TTF (icons/icons.ttf, built in
// Fontello). An icon is a single font glyph rasterised ONCE to a white texture; the draw call tints it, so
// the colour is chosen per-draw and a theme switch (or a per-instance colour like a checkbox border vs
// fill) costs nothing - no re-render. Stays crisp at any size. Any app that links simple-lib-app gets the
// icons for free; no shipped asset. Load the font once with initIconFont(), then build Icons by name from
// ui/icon-ids.h.

#include "font.h"
#include "ui/icon-ids.h"

typedef struct {
   const GfxTexture *tex;    // borrowed from the shared icon-font cache (one texture per id+size, app-wide)
   int    offsetX, offsetY;  // the glyph ink's position within the em box (for font-natural alignment)
   IconId id;
   int    size;              // px the glyph was rasterised at (the font size)
} Icon;

int  initIconFont(void);   // load the embedded font once (idempotent). 0 on success, -1 if unreadable.
void freeIconFont(void);

void initIcon(Icon *icon, IconId id, int size);   // binds the glyph texture once; draw with drawIcon*

// draw the glyph at its natural size, tinted to `color` (its alpha is used). (x, y) is the em-box top-left:
// the glyph is placed within it by the font's metrics, so same-size icons drawn at one anchor line up.
void drawIcon(Icon *icon, int x, int y, uint32_t color);
// as drawIcon but with an explicit alpha (0..255), for fades that override the colour's own alpha.
void drawIconAlpha(Icon *icon, int x, int y, uint32_t color, int alpha);

// draws the glyph with its ink horizontally centred on centerX (y is the em-box top, as for drawIcon).
void drawIconCentered(Icon *icon, int centerX, int y, uint32_t color);

void freeIcon(Icon *icon);
