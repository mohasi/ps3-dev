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
