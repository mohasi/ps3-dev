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

// Offscreen render target (render-to-texture). Draw a scene into one of these instead of
// the display buffer, then sample `tex` like any other texture (cross-fades, post effects,
// UI/scene caching, screenshots). Generic: not tied to any one app.
//   createGfxRenderTarget  allocate an w*h RGBA color surface (0 on success)
//   beginGfxRenderTarget   redirect subsequent draws into rt (flushes the current batch);
//                          drawing coordinates are relative to the target's own w*h
//   endGfxRenderTarget     restore drawing to the frame's display buffer
//   freeGfxRenderTarget    release the surface VRAM
// begin/end do NOT clear -- call clearGfx() after begin if you want a known background.
// Targets are valid between beginGfxFrame/endGfxFrame; do not flip while one is bound.
typedef struct {
   GfxTexture tex;     // sample this after endGfxRenderTarget (tex.offset/w/h/pitch)
} GfxRenderTarget;

int  createGfxRenderTarget(GfxRenderTarget *rt, int w, int h);
void beginGfxRenderTarget(GfxRenderTarget *rt);
void endGfxRenderTarget(void);
void freeGfxRenderTarget(GfxRenderTarget *rt);

// Screenshot: returns a CPU-readable pointer to the FRONT (currently displayed) framebuffer in local
// memory -- the last fully-rendered+flipped frame -- and writes the screen width/height + row pitch
// (bytes). Pixels are A8R8G8B8. Valid until the next flip; NULL if gfx isn't initialised. (On RPCS3
// needs the "Write Color Buffers" GPU option; on real PS3 local memory is readable directly.)
const void *getGfxDisplayBuffer(int *w, int *h, int *pitch);

void beginGfxFrame(void);
void clearGfx(uint32_t argb);
void fillGfxRectangle(int x, int y, int w, int h, uint32_t argb);
void strokeGfxRectangle(int x, int y, int w, int h, int thickness, uint32_t argb);
void drawGfxBox(int x, int y, int w, int h, int thickness, uint32_t fill, uint32_t border);
void fillGfxCircle(int cx, int cy, int radius, uint32_t argb);
void drawGfxTriangle(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2);
void drawGfxLine(int x0, int y0, int x1, int y1, int thickness, uint32_t argb);
void drawGfxTexture(int x, int y, int w, int h, GfxTexture tex, float u0, float v0, float u1, float v1, uint32_t tint, GfxFilter filter);
void endGfxFrame(void);

// Blocks until the RSX has finished all submitted commands. Call before reusing
// or freeing VRAM that a previous frame drew from, so the GPU is never reading
// memory while the CPU overwrites it.
void finishGfx(void);

// Loads a PNG or JPEG file into a VRAM texture (dispatches by extension). Returns a
// zero-initialised texture on failure (check .offset). Implemented in image-loader.c.
GfxTexture loadGfxTexture(const char *path);

// Loads an in-memory PNG/JPEG (format sniffed from magic bytes) into a VRAM texture.
// Returns a zero-initialised texture on failure (check .offset). Implemented in image-loader.c.
GfxTexture loadGfxTextureMem(const void *data, uint32_t size);
uint32_t   uploadGfxTexture(const void *rgba, int w, int h, int srcPitch);
void       updateGfxTexture(uint32_t offset, const void *rgba, int w, int h, int srcPitch, int slotW, int slotH);

// Video frame path (zero-copy). allocGfxVideoBuffer returns main memory mapped for RSX access —
// decode YUV 4:2:0 planar frames (Y then U then V, packed) straight into it, then drawGfxYuvFrame
// samples the planes in place and converts to RGB in a fragment shader. No per-frame copies.
// frameW/frameH are the coded plane dimensions; x/y/w/h the on-screen rectangle.
void *allocGfxVideoBuffer(size_t size);
void  freeGfxVideoBuffer(void *buffer);
void  drawGfxYuvFrame(int x, int y, int w, int h, const void *yuvPlanes, int frameW, int frameH);

int getGfxScreenWidth(void);
int getGfxScreenHeight(void);

// fits a w x h frame inside the screen preserving aspect ratio; returns the centred destination
// rect (the letterbox for a video frame).
static inline void getGfxLetterboxRect(int w, int h, int *dx, int *dy, int *dw, int *dh)
{
   int screenW = getGfxScreenWidth(), screenH = getGfxScreenHeight();
   float frameAspect  = (float)w / (float)h;
   float screenAspect = (float)screenW / (float)screenH;
   if (frameAspect > screenAspect) { *dw = screenW; *dh = (int)(screenW / frameAspect); }
   else                            { *dh = screenH; *dw = (int)(screenH * frameAspect); }
   *dx = (screenW - *dw) / 2;
   *dy = (screenH - *dh) / 2;
}

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

