#pragma once

// line - line UI component

#include "gfx.h"

typedef struct {
    int x0, y0, x1, y1;
    int thickness;
    uint32_t color;
} Line;

static inline void lineInit(Line *l, int x0, int y0, int x1, int y1, int thickness, uint32_t color)
{
    l->x0 = x0; l->y0 = y0;
    l->x1 = x1; l->y1 = y1;
    l->thickness = thickness;
    l->color = color;
}

static inline void lineDraw(Line *l)
{
    gfxDrawLine(l->x0, l->y0, l->x1, l->y1, l->thickness, l->color);
}
