#pragma once

// slice - 3-slice horizontal image that stretches the middle while preserving edge caps

#include "gfx.h"

typedef struct {
    GfxTexture tex;
    int x, y, w, h;
    int capW;
    float v0, v1;
    float uLeft0, uLeft1;
    float uMid0, uMid1;
    float uRight0, uRight1;
} Slice;

static inline void initSlice(Slice *s, GfxTexture tex, int x, int y, int w, int h, SpriteRegion src, int capW)
{
    s->tex = tex;
    s->x = x;
    s->y = y;
    s->w = w;
    s->h = h;
    s->capW = capW;
    s->v0 = (float)src.y / (float)tex.h;
    s->v1 = (float)(src.y + src.h) / (float)tex.h;
    s->uLeft0  = (float)src.x / (float)tex.w;
    s->uLeft1  = (float)(src.x + capW) / (float)tex.w;
    s->uMid0   = (float)(src.x + capW) / (float)tex.w;
    s->uMid1   = (float)(src.x + src.w - capW) / (float)tex.w;
    s->uRight0 = (float)(src.x + src.w - capW) / (float)tex.w;
    s->uRight1 = (float)(src.x + src.w) / (float)tex.w;
}

static inline void moveSlice(Slice *s, int x, int y)
{
    s->x = x;
    s->y = y;
}

static inline void drawSlice(Slice *s)
{
    int midX = s->x + s->capW;
    int midW = s->w - s->capW * 2;
    drawGfxTexture(s->x, s->y, s->capW, s->h, s->tex, s->uLeft0, s->v0, s->uLeft1, s->v1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(midX, s->y, midW, s->h, s->tex, s->uMid0, s->v0, s->uMid1, s->v1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(midX + midW, s->y, s->capW, s->h, s->tex, s->uRight0, s->v0, s->uRight1, s->v1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
}

// nine-slice - preserves all four corners (capW x capH), stretches the four
// edges along one axis and the centre on both. good for rounded panels/borders
// drawn at an arbitrary size from a small source sprite.

typedef struct {
    GfxTexture tex;
    int x, y, w, h;
    int capW, capH;
    float uL0, uL1, uM0, uM1, uR0, uR1;
    float vT0, vT1, vM0, vM1, vB0, vB1;
} NineSlice;

static inline void initNineSlice(NineSlice *s, GfxTexture tex, int x, int y, int w, int h, SpriteRegion src, int capW, int capH)
{
    s->tex = tex;
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->capW = capW; s->capH = capH;
    s->uL0 = (float)src.x              / (float)tex.w;
    s->uL1 = (float)(src.x + capW)     / (float)tex.w;
    s->uM0 = s->uL1;
    s->uM1 = (float)(src.x + src.w - capW) / (float)tex.w;
    s->uR0 = s->uM1;
    s->uR1 = (float)(src.x + src.w)    / (float)tex.w;
    s->vT0 = (float)src.y              / (float)tex.h;
    s->vT1 = (float)(src.y + capH)     / (float)tex.h;
    s->vM0 = s->vT1;
    s->vM1 = (float)(src.y + src.h - capH) / (float)tex.h;
    s->vB0 = s->vM1;
    s->vB1 = (float)(src.y + src.h)    / (float)tex.h;
}

static inline void moveNineSlice(NineSlice *s, int x, int y) { s->x = x; s->y = y; }

static inline void drawNineSlice(NineSlice *s)
{
    int x0 = s->x, x1 = s->x + s->capW, x2 = s->x + s->w - s->capW;
    int y0 = s->y, y1 = s->y + s->capH, y2 = s->y + s->h - s->capH;
    int midW = s->w - s->capW * 2;
    int midH = s->h - s->capH * 2;
    GfxTexture t = s->tex;

    drawGfxTexture(x0, y0, s->capW, s->capH, t, s->uL0, s->vT0, s->uL1, s->vT1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(x1, y0, midW,    s->capH, t, s->uM0, s->vT0, s->uM1, s->vT1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(x2, y0, s->capW, s->capH, t, s->uR0, s->vT0, s->uR1, s->vT1, 0xFFFFFFFF, GFX_FILTER_LINEAR);

    drawGfxTexture(x0, y1, s->capW, midH, t, s->uL0, s->vM0, s->uL1, s->vM1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(x1, y1, midW,    midH, t, s->uM0, s->vM0, s->uM1, s->vM1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(x2, y1, s->capW, midH, t, s->uR0, s->vM0, s->uR1, s->vM1, 0xFFFFFFFF, GFX_FILTER_LINEAR);

    drawGfxTexture(x0, y2, s->capW, s->capH, t, s->uL0, s->vB0, s->uL1, s->vB1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(x1, y2, midW,    s->capH, t, s->uM0, s->vB0, s->uM1, s->vB1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
    drawGfxTexture(x2, y2, s->capW, s->capH, t, s->uR0, s->vB0, s->uR1, s->vB1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
}
