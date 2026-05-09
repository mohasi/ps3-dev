#pragma once

// circle - filled circle UI component with optional border

#include "gfx.h"

typedef struct {
    int cx, cy, radius;
    int borderThickness;
    uint32_t fill;
    uint32_t borderColor;
} Circle;

static inline void circleInit(Circle *c, int cx, int cy, int radius, uint32_t fill, int borderThickness, uint32_t borderColor)
{
    c->cx = cx;
    c->cy = cy;
    c->radius = radius;
    c->fill = fill;
    c->borderThickness = borderThickness;
    c->borderColor = borderColor;
}

static inline void circleDraw(Circle *c)
{
    gfxFillCircle(c->cx, c->cy, c->radius, c->fill, c->borderThickness, c->borderColor);
}
