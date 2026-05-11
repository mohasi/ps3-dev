#pragma once

// rectangle - filled rectangle UI component

#include "gfx.h"

typedef struct {
    int x, y, w, h;
    uint32_t fill;
} Rectangle;

static inline void initRectangle(Rectangle *r, int x, int y, int w, int h, uint32_t fill)
{
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->fill = fill;
}

static inline void moveRectangle(Rectangle *r, int x, int y)
{
    r->x = x;
    r->y = y;
}

static inline void drawRectangle(Rectangle *r)
{
    fillGfxRectangle(r->x, r->y, r->w, r->h, r->fill);
}
