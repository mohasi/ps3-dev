#pragma once

// triangle - filled triangle UI component with optional border

#include "gfx.h"

typedef struct {
    float x0, y0, x1, y1, x2, y2;
    int borderThickness;
    uint32_t fill;
    uint32_t borderColor;
} Triangle;

static inline void triangleInit(Triangle *t, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t fill, int borderThickness, uint32_t borderColor)
{
    t->x0 = x0; t->y0 = y0;
    t->x1 = x1; t->y1 = y1;
    t->x2 = x2; t->y2 = y2;
    t->fill = fill;
    t->borderThickness = borderThickness;
    t->borderColor = borderColor;
}

static inline void triangleDraw(Triangle *t)
{
    gfxDrawTriangle(t->x0, t->y0, t->fill, t->x1, t->y1, t->fill, t->x2, t->y2, t->fill);
    if (t->borderThickness > 0)
    {
        gfxDrawLine((int)t->x0, (int)t->y0, (int)t->x1, (int)t->y1, t->borderThickness, t->borderColor);
        gfxDrawLine((int)t->x1, (int)t->y1, (int)t->x2, (int)t->y2, t->borderThickness, t->borderColor);
        gfxDrawLine((int)t->x2, (int)t->y2, (int)t->x0, (int)t->y0, t->borderThickness, t->borderColor);
    }
}
