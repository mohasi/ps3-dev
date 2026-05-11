#pragma once

// image - positioned texture wrapper

#include "gfx.h"

typedef struct {
    GfxTexture tex;
    GfxFilter filter;
    int x, y, w, h;
    float u0, v0, u1, v1;
} Image;

static inline void initImage(Image *img, GfxTexture tex, int x, int y, int w, int h, int texX, int texY, int texW, int texH, GfxFilter filter)
{
    img->tex = tex;
    img->filter = filter;
    img->x = x;
    img->y = y;
    if (texW > 0 && texH > 0) {
        img->w = w > 0 ? w : texW;
        img->h = h > 0 ? h : texH;
        img->u0 = (float)texX / (float)tex.w;
        img->v0 = (float)texY / (float)tex.h;
        img->u1 = (float)(texX + texW) / (float)tex.w;
        img->v1 = (float)(texY + texH) / (float)tex.h;
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

static inline void drawImage(Image *img)
{
    drawGfxTexture(img->x, img->y, img->w, img->h, img->tex, img->u0, img->v0, img->u1, img->v1, 0xFFFFFFFF, img->filter);
}
