#pragma once

// triangle - filled triangle UI component

#include "gfx.h"

typedef struct {
   float x0, y0, x1, y1, x2, y2;
   uint32_t fill;
} Triangle;

static inline void initTriangle(Triangle *t, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t fill)
{
   t->x0 = x0; t->y0 = y0;
   t->x1 = x1; t->y1 = y1;
   t->x2 = x2; t->y2 = y2;
   t->fill = fill;
}

static inline void moveTriangle(Triangle *t, float x0, float y0, float x1, float y1, float x2, float y2)
{
   t->x0 = x0; t->y0 = y0;
   t->x1 = x1; t->y1 = y1;
   t->x2 = x2; t->y2 = y2;
}

static inline void drawTriangle(Triangle *t)
{
   drawGfxTriangle(t->x0, t->y0, t->fill, t->x1, t->y1, t->fill, t->x2, t->y2, t->fill);
}
