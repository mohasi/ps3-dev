#pragma once

// checkbox - flat/metro square, drawn in code by state: empty = border only, checked = accent-filled.

#include "gfx.h"

typedef struct {
   int x, y, size;
   uint32_t borderColor, fillColor;
} Checkbox;

static inline void initCheckbox(Checkbox *cb, int x, int y, int size, uint32_t borderColor, uint32_t fillColor)
{
   cb->x = x;
   cb->y = y;
   cb->size = size;
   cb->borderColor = borderColor;
   cb->fillColor = fillColor;
}

static inline void moveCheckbox(Checkbox *cb, int x, int y)
{
   cb->x = x;
   cb->y = y;
}

static inline void drawCheckboxAlpha(Checkbox *cb, int isChecked, int alpha)
{
   uint32_t border = (cb->borderColor & 0x00FFFFFFu) | ((uint32_t)alpha << 24);
   strokeGfxRectangle(cb->x, cb->y, cb->size, cb->size, 2, border);
   if (isChecked) {
      uint32_t fill = (cb->fillColor & 0x00FFFFFFu) | ((uint32_t)alpha << 24);
      fillGfxRectangle(cb->x + 4, cb->y + 4, cb->size - 8, cb->size - 8, fill);   // inset so the border stays visible
   }
}
