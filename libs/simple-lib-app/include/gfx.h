#pragma once

// 2D graphics over libgcm/RSX.
// Color format: 0xAARRGGBB. Origin at top-left.

#include <stdint.h>
#include <stddef.h>

#define AUTO 0

typedef enum {
    GFX_FILTER_NEAREST = 0, // sharp, pixelated - good for pixel art and sprites
    GFX_FILTER_LINEAR  = 1  // smooth, blended - good for text and photos
} GfxFilter;

typedef enum {
    GFX_VSYNC_OFF = 0, // uncapped (HSYNC)
    GFX_VSYNC_ON  = 1  // locked to display refresh
} GfxVsync;

typedef struct {
    uint32_t offset;
    int w, h;
    int pitch; // vram row stride in bytes (may be wider than w*4 due to alignment or slot reuse)
} GfxTexture;

typedef struct {
    int x, y, w, h;
} SpriteRegion;

int  initGfx(GfxVsync vsync);
void termGfx(void);

void beginGfxFrame(void);
void clearGfx(uint32_t argb);
void fillGfxRectangle(int x, int y, int w, int h, uint32_t argb);
void fillGfxCircle(int cx, int cy, int radius, uint32_t argb);
void drawGfxTriangle(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2);
void drawGfxLine(int x0, int y0, int x1, int y1, int thickness, uint32_t argb);
void drawGfxTexture(int x, int y, int w, int h, GfxTexture tex, float u0, float v0, float u1, float v1, uint32_t tint, GfxFilter filter);
void endGfxFrame(void);

// Blocks until the RSX has finished all submitted commands. Call before reusing
// or freeing VRAM that a previous frame drew from, so the GPU is never reading
// memory while the CPU overwrites it.
void finishGfx(void);

GfxTexture loadGfxTexture(const char *path);
uint32_t   uploadGfxTexture(const void *rgba, int w, int h, int srcPitch);
void       updateGfxTexture(uint32_t offset, const void *rgba, int w, int h, int srcPitch, int slotW, int slotH);

int getGfxScreenWidth(void);
int getGfxScreenHeight(void);

// free-list VRAM allocator. allocateVram returns an aligned pointer into RSX
// local memory; freeVram releases it and coalesces adjacent free space. Each
// owner frees what it allocated (no global mark/reset).
void      *allocateVram(size_t size, size_t alignment);
void       freeVram(void *ptr);
size_t     getUsedVram(void);            // bytes currently allocated
size_t     getFreeVram(void);            // total free bytes (may be fragmented)
size_t     getLargestFreeBlock(void);    // largest single free run

// frees the VRAM backing a texture and zeroes it. safe on a zeroed/empty tex.
void       freeGfxTexture(GfxTexture *tex);

