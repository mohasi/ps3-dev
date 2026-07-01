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
   if (tex.w <= 0 || tex.h <= 0) {
      s->v0 = s->v1 = 0.0f;
      s->uLeft0 = s->uLeft1 = s->uMid0 = s->uMid1 = s->uRight0 = s->uRight1 = 0.0f;
      return;
   }
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

// nine-slice - preserves all four corners, stretches the four edges along one
// axis and the centre on both. good for rounded panels/borders drawn at an
// arbitrary size from a small source sprite.
//
// the corner caps have separate SOURCE and DEST sizes: capW/capH select the
// pixels in the source region, dstCapW/dstCapH are the size those caps are
// drawn at. initNineSlice keeps them equal (caps drawn at native pixel size,
// the classic UI case). initNineSliceScaled sets dstCap = cap * scale, which
// reproduces "compose at native size, then scale the whole surface" (UIs
// authored for a fixed design resolution): the caps scale by the same ratio
// as the centre.

typedef struct {
   GfxTexture tex;
   int x, y, w, h;
   int capW, capH;        // source cap size (px of the source region)
   int dstCapW, dstCapH;  // drawn cap size
   uint32_t tint;         // colour multiplied into the texture (0xFFFFFFFF = unchanged);
                     // lets a grayscale template be recoloured, like Ren'Py OneOrTwoColor
   float uL0, uL1, uM0, uM1, uR0, uR1;
   float vT0, vT1, vM0, vM1, vB0, vB1;
} NineSlice;

static inline void initNineSlice(NineSlice *s, GfxTexture tex, int x, int y, int w, int h, SpriteRegion src, int capW, int capH)
{
   s->tex = tex;
   s->x = x; s->y = y; s->w = w; s->h = h;
   s->capW = capW; s->capH = capH;
   s->dstCapW = capW; s->dstCapH = capH;
   s->tint = 0xFFFFFFFFu;
   if (tex.w <= 0 || tex.h <= 0) {
      s->uL0 = s->uL1 = s->uM0 = s->uM1 = s->uR0 = s->uR1 = 0.0f;
      s->vT0 = s->vT1 = s->vM0 = s->vM1 = s->vB0 = s->vB1 = 0.0f;
      return;
   }
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

// As initNineSlice, but the caps are drawn at cap * scale pixels (see header note).
static inline void initNineSliceScaled(NineSlice *s, GfxTexture tex, int x, int y, int w, int h, SpriteRegion src, int capW, int capH, float scale)
{
   initNineSlice(s, tex, x, y, w, h, src, capW, capH);
   s->dstCapW = (int)(capW * scale + 0.5f);
   s->dstCapH = (int)(capH * scale + 0.5f);
}

static inline void moveNineSlice(NineSlice *s, int x, int y) { s->x = x; s->y = y; }

static inline void drawNineSlice(NineSlice *s)
{
   // clamp the drawn caps so they never overlap on a small dest rect
   int cw = s->dstCapW, ch = s->dstCapH;
   if (cw * 2 > s->w) cw = s->w / 2;
   if (ch * 2 > s->h) ch = s->h / 2;

   int x0 = s->x, x1 = s->x + cw, x2 = s->x + s->w - cw;
   int y0 = s->y, y1 = s->y + ch, y2 = s->y + s->h - ch;
   int midW = s->w - cw * 2;
   int midH = s->h - ch * 2;
   GfxTexture tex = s->tex;
   uint32_t tint = s->tint;

   drawGfxTexture(x0, y0, cw,   ch, tex, s->uL0, s->vT0, s->uL1, s->vT1, tint, GFX_FILTER_LINEAR);
   drawGfxTexture(x1, y0, midW, ch, tex, s->uM0, s->vT0, s->uM1, s->vT1, tint, GFX_FILTER_LINEAR);
   drawGfxTexture(x2, y0, cw,   ch, tex, s->uR0, s->vT0, s->uR1, s->vT1, tint, GFX_FILTER_LINEAR);

   drawGfxTexture(x0, y1, cw,   midH, tex, s->uL0, s->vM0, s->uL1, s->vM1, tint, GFX_FILTER_LINEAR);
   drawGfxTexture(x1, y1, midW, midH, tex, s->uM0, s->vM0, s->uM1, s->vM1, tint, GFX_FILTER_LINEAR);
   drawGfxTexture(x2, y1, cw,   midH, tex, s->uR0, s->vM0, s->uR1, s->vM1, tint, GFX_FILTER_LINEAR);

   drawGfxTexture(x0, y2, cw,   ch, tex, s->uL0, s->vB0, s->uL1, s->vB1, tint, GFX_FILTER_LINEAR);
   drawGfxTexture(x1, y2, midW, ch, tex, s->uM0, s->vB0, s->uM1, s->vB1, tint, GFX_FILTER_LINEAR);
   drawGfxTexture(x2, y2, cw,   ch, tex, s->uR0, s->vB0, s->uR1, s->vB1, tint, GFX_FILTER_LINEAR);
}
