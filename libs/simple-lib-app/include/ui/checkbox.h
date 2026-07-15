#pragma once

// checkbox - flat/metro checkbox drawn from the icon font: the empty-box glyph when unchecked, the
// box-with-tick glyph when checked. Both share the same square in the font, so the box stays put and the
// tick overhangs naturally. One colour for the whole control (`color`), which follows the theme. Glyphs
// are rasterised once (initCheckboxIcons, after initIconFont) and tinted per-checkbox at draw.

#include "gfx.h"

typedef struct {
   int x, y;
   uint32_t color;
} Checkbox;

static inline void initCheckbox(Checkbox *cb, int x, int y, uint32_t color)
{
   cb->x = x;
   cb->y = y;
   cb->color = color;
}

static inline void moveCheckbox(Checkbox *cb, int x, int y)
{
   cb->x = x;
   cb->y = y;
}

void initCheckboxIcons(void);   // rasterise the box + tick glyphs once (needs the icon font loaded)
void freeCheckboxIcons(void);
void drawCheckboxAlpha(Checkbox *cb, int isChecked, int alpha);
