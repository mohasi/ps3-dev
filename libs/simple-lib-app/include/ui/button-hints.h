#pragma once

// button-hints - a horizontally-centered row of "[glyph] caption" pad hints for the bottom of a screen
// (e.g. [X] Play  [O] Back). The glyphs are textures the caller supplies - pair with console-glyphs.h to get
// the console's own button art. draw-only: the owning screen still reads the pad. build the row once, then
// drawButtonHints each frame.

#include "font.h"
#include "gfx.h"
#include "ui/label.h"

#define MAX_BUTTON_HINTS 16

typedef struct {
   GfxTexture glyph;
   int        drawWidth;   // glyph width scaled to the row's glyph height (aspect preserved)
   Label      caption;
   int        width;       // full width of this hint, precomputed
   int        pairNext;    // caption-less glyph: cluster tightly with the next hint (e.g. L1 + R1)
} ButtonHint;

typedef struct {
   Font    *font;
   int      y, glyphHeight, captionSize;
   uint32_t captionColor;
   ButtonHint hints[MAX_BUTTON_HINTS];
   int      count;
} ButtonHints;

void initButtonHints(ButtonHints *bar, Font *font, int y, int glyphHeight, int captionSize, uint32_t captionColor);
// returns the index of the hint just added, for later setButtonHintCaption calls (-1 when the row is full).
int addButtonHint(ButtonHints *bar, GfxTexture glyph, const char *caption);
// retitle an existing hint (e.g. Subscribe <-> Unsubscribe); recomputes its width so the row stays centered.
void setButtonHintCaption(ButtonHints *bar, int index, const char *caption);
void drawButtonHints(ButtonHints *bar, int screenWidth);
void termButtonHints(ButtonHints *bar);
