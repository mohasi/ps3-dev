#pragma once

// 2D graphics over libgcm/RSX.
// Color format: 0xAARRGGBB. Origin at top-left.

#include <stdint.h>
#include <stddef.h>

#define AUTO 0
#define NO_BORDER    0, 0
#define NO_ROUNDING  0

typedef enum {
    GFX_FILTER_NEAREST = 0, // sharp, pixelated - good for pixel art and sprites
    GFX_FILTER_LINEAR  = 1  // smooth, blended - good for text and photos
} GfxFilter;

typedef enum {
    GFX_VSYNC_OFF = 0, // uncapped (HSYNC)
    GFX_VSYNC_ON  = 1  // locked to display refresh
} GfxVsync;

typedef enum {
    VRAM_PERM = 0, // lives until screen terms
    VRAM_TEMP = 1  // reset every frame
} VramLifetime;

typedef struct {
    uint32_t offset;
    int w, h;
} GfxTexture;

int  gfxInit(GfxVsync vsync);
void gfxTerm(void);

void gfxBeginFrame(void);
void gfxClear(uint32_t argb);
void gfxFillRectangle(int x, int y, int w, int h, uint32_t argb);
void gfxFillRoundedRectangle(int x, int y, int w, int h, int radius, uint32_t argb, int borderThickness, uint32_t borderArgb);
void gfxFillCircle(int cx, int cy, int radius, uint32_t argb, int borderThickness, uint32_t borderArgb);
void gfxDrawTriangle(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2);
void gfxDrawLine(int x0, int y0, int x1, int y1, int thickness, uint32_t argb);
void gfxDrawTexture(int x, int y, int w, int h, GfxTexture tex, float u0, float v0, float u1, float v1, uint32_t tint, GfxFilter filter);
void gfxEndFrame(void);

GfxTexture gfxLoadTexture(const char *path);
uint32_t   gfxUploadTexture(const void *rgba, int w, int h, int srcPitch, VramLifetime lifetime);

int gfxScreenWidth(void);
int gfxScreenHeight(void);

void  *vramAlloc(size_t size, size_t alignment, VramLifetime lifetime);
void   gfxVramReset(size_t mark);
size_t gfxVramUsed(void);
size_t gfxVramUsedTemp(void);


