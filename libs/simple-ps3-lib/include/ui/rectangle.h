#pragma once

// rectangle - filled rectangle UI component with optional rounding and border

#include "gfx.h"

typedef struct {
    int x, y, w, h;
    int radius;
    int borderThickness;
    uint32_t fill;
    uint32_t borderColor;
} Rectangle;

static inline void rectangleInit(Rectangle *r, int x, int y, int w, int h, int radius, uint32_t fill, int borderThickness, uint32_t borderColor)
{
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->radius = radius;
    r->fill = fill;
    r->borderThickness = borderThickness;
    r->borderColor = borderColor;
}

static inline void rectangleDraw(Rectangle *r)
{
    int t = r->borderThickness;
    if (r->radius > 0)
    {
        if (t > 0)
        {
            gfxFillRoundedRectangle(r->x, r->y, r->w, r->h, r->radius, r->borderColor);
            gfxFillRoundedRectangle(r->x + t, r->y + t, r->w - 2 * t, r->h - 2 * t, r->radius - t, r->fill);
        }
        else
        {
            gfxFillRoundedRectangle(r->x, r->y, r->w, r->h, r->radius, r->fill);
        }
    }
    else
    {
        if (t > 0)
        {
            gfxFillRectangle(r->x, r->y, r->w, r->h, r->borderColor);
            gfxFillRectangle(r->x + t, r->y + t, r->w - 2 * t, r->h - 2 * t, r->fill);
        }
        else
        {
            gfxFillRectangle(r->x, r->y, r->w, r->h, r->fill);
        }
    }
}
