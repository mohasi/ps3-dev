#pragma once

// image - positioned texture wrapper

#include "gfx.h"

typedef struct {
   GfxTexture tex;
   GfxFilter filter;
   int x, y, w, h;
   float u0, v0, u1, v1;
} Image;

static inline void initImage(Image *img, GfxTexture tex, int x, int y, int w, int h, SpriteRegion src, GfxFilter filter)
{
   img->tex = tex;
   img->filter = filter;
   img->x = x;
   img->y = y;
   if (src.w > 0 && src.h > 0 && tex.w > 0 && tex.h > 0) {
      img->w = w > 0 ? w : src.w;
      img->h = h > 0 ? h : src.h;
      img->u0 = (float)src.x / (float)tex.w;
      img->v0 = (float)src.y / (float)tex.h;
      img->u1 = (float)(src.x + src.w) / (float)tex.w;
      img->v1 = (float)(src.y + src.h) / (float)tex.h;
   } else {
      img->w = w > 0 ? w : tex.w;
      img->h = h > 0 ? h : tex.h;
      img->u0 = 0.0f;
      img->v0 = 0.0f;
      img->u1 = 1.0f;
      img->v1 = 1.0f;
   }
}

static inline void moveImage(Image *img, int x, int y)
{
   img->x = x;
   img->y = y;
}

// draws the image modulated by alpha (0-255); RGB stays untouched so this
// just controls overall opacity (e.g. a 50% "ghosted" look for cut items).
static inline void drawImageAlpha(Image *img, int alpha)
{
   uint32_t tint = ((uint32_t)(alpha & 0xFF) << 24) | 0x00FFFFFF;
   drawGfxTexture(img->x, img->y, img->w, img->h, img->tex, img->u0, img->v0, img->u1, img->v1, tint, img->filter);
}

static inline void drawImage(Image *img) { drawImageAlpha(img, 0xFF); }

// moves then draws in one call - handy when an image is repositioned every frame.
static inline void drawImageAt(Image *img, int x, int y) { moveImage(img, x, y); drawImage(img); }
