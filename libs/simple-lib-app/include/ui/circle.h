#pragma once

// circle - filled circle UI component

#include "gfx.h"

typedef struct {
    int cx, cy, radius;
    uint32_t fill;
} Circle;

static inline void initCircle(Circle *c, int cx, int cy, int radius, uint32_t fill)
{
    c->cx = cx;
    c->cy = cy;
    c->radius = radius;
    c->fill = fill;
}

static inline void moveCircle(Circle *c, int cx, int cy)
{
    c->cx = cx;
    c->cy = cy;
}

static inline void drawCircle(Circle *c)
{
    fillGfxCircle(c->cx, c->cy, c->radius, c->fill);
}
