#pragma once

// checkbox - pre-rendered checked/unchecked image pair, drawn by state

#include "ui/image.h"

typedef struct {
   Image unchecked, checked;
} Checkbox;

static inline void initCheckbox(Checkbox *cb, GfxTexture sprites, int x, int y, int size,
                                 SpriteRegion uncheckedSprite, SpriteRegion checkedSprite)
{
   initImage(&cb->unchecked, sprites, x, y, size, size, uncheckedSprite, GFX_FILTER_LINEAR);
   initImage(&cb->checked,   sprites, x, y, size, size, checkedSprite,   GFX_FILTER_LINEAR);
}

static inline void moveCheckbox(Checkbox *cb, int x, int y)
{
   moveImage(&cb->unchecked, x, y);
   moveImage(&cb->checked, x, y);
}

static inline void drawCheckboxAlpha(Checkbox *cb, int isChecked, int alpha)
{
   drawImageAlpha(isChecked ? &cb->checked : &cb->unchecked, alpha);
}
