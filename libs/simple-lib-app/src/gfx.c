// 2D graphics implementation using libgcm/RSX.
// Triple-buffered, uncapped. Shader-based batched quad renderer.
#include "gfx.h"
#include "colors.h"
#include "dbg.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <sys/timer.h>
#include <sys/sys_time.h>
#include <cell/gcm.h>
#include <cell/codec/pngdec.h>
#include <cell/sysmodule.h>
#include <sysutil/sysutil_sysparam.h>

#define CTX gCellGcmCurrentContext

#define FRAME_COUNT       3
#define HOST_MEM_SIZE     (1 * 1024 * 1024)
#define CMD_BUFFER_SIZE   (0x10000)
#define BYTES_PER_PIXEL   4

// max quads per frame (6 verts each). the batch costs 24 bytes per vertex in two
// buffers, so this is 4.5 MB of the console's ~250 MB of graphics memory - cheap
// enough that no screen in the tree should ever have to draw around it.
#define MAX_QUADS         16384
#define MAX_VERTS         (MAX_QUADS * 6)

// what each shape costs out of that budget. composite shapes reserve their whole
// cost before drawing any part, so a shape at the cap goes missing rather than
// coming out half drawn.
#define VERTS_PER_TRIANGLE     3
#define VERTS_PER_RECT         6
#define VERTS_PER_LINE         (VERTS_PER_TRIANGLE * 2)
#define VERTS_PER_STROKED_RECT (VERTS_PER_RECT * 4)

// RSX hard limit on texture/surface dimensions (mirrors image-loader.c's MAX_TEX_DIM)
#define MAX_TEX_DIM       4096

typedef struct {
   float x, y, z;
   uint32_t rgba;
   float u, v;
} GfxVertex;

static int       initialized = 0;
static int       screenW = 0;        // display buffer dimensions (the flipped surface)
static int       screenH = 0;
// Dimensions of the CURRENT render target (the display buffer, or a bound offscreen
// target). All drawing math (clip-space, clamping, viewport, scissor) uses these so the
// same draw calls work whether we're rendering to screen or to a texture.
static int       targetW = 0;
static int       targetH = 0;
static uint32_t  framePitch = 0;
static uint32_t  frameOffset[FRAME_COUNT];
static uint32_t  backBuffer = 0;

static uint8_t  *vramBase = 0;
static size_t    vramSize = 0;

// free-list allocator state. The list tiles the whole VRAM region contiguously
// in address order, so list-adjacent blocks are memory-adjacent and can be
// coalesced on free. Node metadata lives in main RAM (malloc), not VRAM.
typedef struct VramBlock {
   void   *addr;   // start address of this block
   size_t  size;   // span of this block in bytes (== memory it covers)
   int     isFree; // 1 if free, 0 if allocated
   struct VramBlock *next;
} VramBlock;

static VramBlock *blockListHead = NULL;
static size_t     vramAllocated = 0;  // sum of allocated block sizes

// double-buffered vertex batch (write one while GPU reads the other)
static GfxVertex *batchVerts[FRAME_COUNT];
static uint32_t   batchOffset[FRAME_COUNT];
static uint32_t   batchColOffset[FRAME_COUNT];
static uint32_t   batchUvOffset[FRAME_COUNT];
static int        batchVertCount = 0;
static int        batchFlushStart = 0;
static int        frameDroppedCalls = 0;     // draw calls the frame could not afford

// shader state
extern struct _CGprogram _binary_vpshader_vpo_start;
extern struct _CGprogram _binary_fpshader_fpo_start;
extern struct _CGprogram _binary_fpshader_yuv_fpo_start;

static CGprogram shaderVp;
static CGprogram shaderFp;
static void     *shaderVpUcode;
static void     *shaderFpUcode;
static uint32_t  shaderFpOffset;
static uint32_t  shaderPosIdx;
static uint32_t  shaderColIdx;
static uint32_t  shaderUvIdx;
static uint32_t  shaderTexUnit;

// yuv video shader: converts YUV 4:2:0 planar frames to RGB on the RSX (see fpshader-yuv.cg)
static CGprogram shaderFpYuv;
static uint32_t  shaderFpYuvOffset;
static uint32_t  shaderYuvTexUnit[3];   // texY, texU, texV

// 1x1 white texture for color-only draws
static uint32_t whiteTexOffset = 0;

// current batch texture state
static uint32_t batchTexOffset = 0;
static int      batchTexW = 1;
static int      batchTexH = 1;
static int      batchTexPitch = 64;
static int      batchTexLinear = 0;

static void flushBatch(void);

void *allocateVram(size_t size, size_t alignment)
{
   if (size == 0) return NULL;
   if (alignment == 0) alignment = 1;

   // first call: one free block covering the whole region.
   if (blockListHead == NULL) {
      blockListHead = (VramBlock *)malloc(sizeof(VramBlock));
      if (!blockListHead) return NULL;
      blockListHead->addr = vramBase;
      blockListHead->size = vramSize;
      blockListHead->isFree = 1;
      blockListHead->next = NULL;
   }

   VramBlock *prev = NULL;
   for (VramBlock *block = blockListHead; block; prev = block, block = block->next) {
      if (!block->isFree) continue;

      size_t base    = (size_t)block->addr;
      size_t aligned = (base + alignment - 1) & ~(alignment - 1);
      size_t padding = aligned - base;
      if (padding + size > block->size) continue;  // doesn't fit

      // carve the alignment padding off the front as its own free block,
      // linked in just before this one (prev is the predecessor) so the list
      // stays address-ordered.
      if (padding > 0) {
         VramBlock *padBlock = (VramBlock *)malloc(sizeof(VramBlock));
         if (!padBlock) return NULL;
         padBlock->addr = block->addr;
         padBlock->size = padding;
         padBlock->isFree = 1;
         padBlock->next = block;
         if (prev) prev->next = padBlock;
         else      blockListHead = padBlock;
         block->addr  = (void *)aligned;
         block->size -= padding;
      }

      // carve any leftover off the back as its own free block.
      if (block->size > size) {
         VramBlock *tailBlock = (VramBlock *)malloc(sizeof(VramBlock));
         if (!tailBlock) return NULL;
         tailBlock->addr = (uint8_t *)block->addr + size;
         tailBlock->size = block->size - size;
         tailBlock->isFree = 1;
         tailBlock->next = block->next;
         block->next = tailBlock;
         block->size = size;
      }

      block->isFree = 0;
      vramAllocated += block->size;
      return block->addr;
   }

   logError("[gfx] allocateVram: out of VRAM (%zu bytes)\n", size);
   return NULL;
}

void freeVram(void *ptr)
{
   if (!ptr) return;

   VramBlock *prev = NULL;
   for (VramBlock *block = blockListHead; block; prev = block, block = block->next) {
      if (block->addr != ptr) continue;
      if (block->isFree) return;  // double-free: ignore

      block->isFree = 1;
      vramAllocated -= block->size;

      // merge with the following block if it is free (memory-adjacent).
      if (block->next && block->next->isFree) {
         VramBlock *next = block->next;
         block->size += next->size;
         block->next  = next->next;
         free(next);
      }

      // merge into the preceding block if it is free.
      if (prev && prev->isFree) {
         prev->size += block->size;
         prev->next  = block->next;
         free(block);
      }
      return;
   }
}

// how long the last endGfxFrame spent blocked before it could reuse a buffer. triple-buffered and
// non-blocking, so the CPU stalls here only when the display pipeline is full (the GPU has fallen a
// whole frame behind); in the steady state it is ~0.
static uint64_t lastFlipWaitUs;

// the display pipeline. flipsSubmitted counts flips the CPU has queued; flipsCompleted counts the
// ones the RSX has retired (bumped by the flip callback). their difference is how many are in flight.
static uint32_t          flipsSubmitted;
static volatile uint32_t flipsCompleted;
static void onGfxFlip(const uint32_t head) { (void)head; flipsCompleted++; }

uint64_t getGfxFlipWaitUs(void)
{
   return lastFlipWaitUs;
}

// Binds a linear (CELL_GCM_SURFACE_PITCH) RGBA color surface as the render target and
// records its size as the current target. Works for both the display buffer and an
// offscreen texture (offset/pitch 64-byte aligned). Sets the viewport + scissor to match.
static void setSurface(uint32_t offset, uint32_t pitch, int w, int h)
{
   CellGcmSurface sf;
   memset(&sf, 0, sizeof(sf));

   sf.colorFormat      = CELL_GCM_SURFACE_A8R8G8B8;
   sf.colorTarget      = CELL_GCM_SURFACE_TARGET_0;
   sf.colorLocation[0] = CELL_GCM_LOCATION_LOCAL;
   sf.colorOffset[0]   = offset;
   sf.colorPitch[0]    = pitch;

   // unused color attachments need non-zero pitch
   sf.colorLocation[1] = CELL_GCM_LOCATION_LOCAL;
   sf.colorLocation[2] = CELL_GCM_LOCATION_LOCAL;
   sf.colorLocation[3] = CELL_GCM_LOCATION_LOCAL;
   sf.colorPitch[1]    = 64;
   sf.colorPitch[2]    = 64;
   sf.colorPitch[3]    = 64;

   // no depth buffer (2D only)
   sf.depthFormat      = CELL_GCM_SURFACE_Z16;
   sf.depthLocation    = CELL_GCM_LOCATION_LOCAL;
   sf.depthOffset      = 0;
   sf.depthPitch       = 64;

   sf.type      = CELL_GCM_SURFACE_PITCH;
   sf.antialias = CELL_GCM_SURFACE_CENTER_1;
   sf.width     = w;
   sf.height    = h;
   sf.x         = 0;
   sf.y         = 0;

   cellGcmSetSurface(CTX, &sf);

   targetW = w;
   targetH = h;

   float scale[4]  = { w * 0.5f,  h * -0.5f, 0.5f, 0.0f };
   float offs[4]   = { w * 0.5f,  h *  0.5f, 0.5f, 0.0f };
   cellGcmSetViewport(CTX, 0, 0, w, h, 0.0f, 1.0f, scale, offs);
   cellGcmSetScissor(CTX, 0, 0, w, h);
}

static void setRenderTarget(void)
{
   setSurface(frameOffset[backBuffer], framePitch, screenW, screenH);
}

int initGfx(GfxVsync vsync)
{
   int ret;

   if (initialized) {
      return 0;
   }

   cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);
   cellSysmoduleLoadModule(CELL_SYSMODULE_JPGDEC);

   // allocate host-side command buffer pool
   void *hostAddr = memalign(1024 * 1024, HOST_MEM_SIZE);
   if (!hostAddr) {
      logError("[gfx] memalign host failed\n");
      return -1;
   }

   ret = cellGcmInit(CMD_BUFFER_SIZE, HOST_MEM_SIZE, hostAddr);
   if (ret < 0) {
      logError("[gfx] cellGcmInit failed: 0x%08x\n", ret);
      return ret;
   }

   // read current video mode
   CellVideoOutState videoState;
   ret = cellVideoOutGetState(CELL_VIDEO_OUT_PRIMARY, 0, &videoState);
   if (ret < 0) return ret;

   CellVideoOutResolution res;
   ret = cellVideoOutGetResolution(videoState.displayMode.resolutionId, &res);
   if (ret < 0) return ret;

   screenW = res.width;
   screenH = res.height;
   framePitch = cellGcmGetTiledPitchSize(cellGcmAlign(CELL_GCM_ZCULL_ALIGN_WIDTH, screenW) * BYTES_PER_PIXEL);

   // configure video output
   CellVideoOutConfiguration videoCfg;
   memset(&videoCfg, 0, sizeof(videoCfg));
   videoCfg.resolutionId = videoState.displayMode.resolutionId;
   videoCfg.format       = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
   videoCfg.pitch        = framePitch;

   ret = cellVideoOutConfigure(CELL_VIDEO_OUT_PRIMARY, &videoCfg, 0, 0);
   if (ret < 0) {
      logError("[gfx] cellVideoOutConfigure: 0x%08x\n", ret);
      return ret;
   }

   setGfxVsync(vsync);

   // set up vram allocator
   CellGcmConfig config;
   cellGcmGetConfiguration(&config);
   vramBase = (uint8_t *)config.localAddress;
   vramSize = (size_t)config.localSize;

   // allocate frame buffers in vram
   uint32_t bufferHeight = cellGcmAlign(CELL_GCM_ZCULL_ALIGN_HEIGHT, screenH);
   uint32_t colorSize    = framePitch * bufferHeight;

   for (int i = 0; i < FRAME_COUNT; ++i) {
      void *addr = allocateVram(colorSize, CELL_GCM_TILE_ALIGN_OFFSET);
      if (!addr) {
         return -1;
      }

      ret = cellGcmAddressToOffset(addr, &frameOffset[i]);
      if (ret < 0) {
         logError("[gfx] cellGcmAddressToOffset[%d]: 0x%08x\n", i, ret);
         return ret;
      }

      cellGcmSetTileInfo(i, CELL_GCM_LOCATION_LOCAL, frameOffset[i], colorSize, framePitch, CELL_GCM_COMPMODE_DISABLED, 0, 0);
      cellGcmBindTile(i);

      ret = cellGcmSetDisplayBuffer(i, frameOffset[i], framePitch, screenW, screenH);
      if (ret < 0) {
         logError("[gfx] cellGcmSetDisplayBuffer[%d]: 0x%08x\n", i, ret);
         return ret;
      }
   }

   backBuffer = 0;
   flipsSubmitted = 0;
   flipsCompleted = 0;
   cellGcmSetFlipHandler(onGfxFlip);   // retires in-flight flips so endGfxFrame need not block on them
   initialized = 1;

   // init shaders
   shaderVp = (CGprogram)&_binary_vpshader_vpo_start;
   cellGcmCgInitProgram(shaderVp);
   uint32_t vpSize;
   cellGcmCgGetUCode(shaderVp, &shaderVpUcode, &vpSize);

   shaderFp = (CGprogram)&_binary_fpshader_fpo_start;
   cellGcmCgInitProgram(shaderFp);
   void *fpUcode;
   uint32_t fpSize;
   cellGcmCgGetUCode(shaderFp, &fpUcode, &fpSize);

   shaderFpUcode = allocateVram(fpSize, 64);
   if (!shaderFpUcode) return -1;
   memcpy(shaderFpUcode, fpUcode, fpSize);
   cellGcmAddressToOffset(shaderFpUcode, &shaderFpOffset);

   CGparameter posParam = cellGcmCgGetNamedParameter(shaderVp, "position");
   CGparameter colParam = cellGcmCgGetNamedParameter(shaderVp, "color");
   CGparameter uvParam  = cellGcmCgGetNamedParameter(shaderVp, "texcoord");
   shaderPosIdx = cellGcmCgGetParameterResource(shaderVp, posParam) - CG_ATTR0;
   shaderColIdx = cellGcmCgGetParameterResource(shaderVp, colParam) - CG_ATTR0;
   shaderUvIdx  = cellGcmCgGetParameterResource(shaderVp, uvParam)  - CG_ATTR0;

   CGparameter texParam = cellGcmCgGetNamedParameter(shaderFp, "tex");
   shaderTexUnit = cellGcmCgGetParameterResource(shaderFp, texParam) - CG_TEXUNIT0;

   // yuv video shader
   shaderFpYuv = (CGprogram)&_binary_fpshader_yuv_fpo_start;
   cellGcmCgInitProgram(shaderFpYuv);
   void *fpYuvUcode;
   uint32_t fpYuvSize;
   cellGcmCgGetUCode(shaderFpYuv, &fpYuvUcode, &fpYuvSize);
   void *fpYuvVram = allocateVram(fpYuvSize, 64);
   if (!fpYuvVram) return -1;
   memcpy(fpYuvVram, fpYuvUcode, fpYuvSize);
   cellGcmAddressToOffset(fpYuvVram, &shaderFpYuvOffset);

   static const char *planeNames[3] = { "texY", "texU", "texV" };
   for (int i = 0; i < 3; i++) {
      CGparameter planeParam = cellGcmCgGetNamedParameter(shaderFpYuv, planeNames[i]);
      shaderYuvTexUnit[i] = cellGcmCgGetParameterResource(shaderFpYuv, planeParam) - CG_TEXUNIT0;
   }

   // 1x1 white texture (64-byte pitch minimum for RSX)
   uint32_t *whitePix = (uint32_t *)allocateVram(64, 64);
   if (!whitePix) return -1;
   *whitePix = COLOR_WHITE;
   cellGcmAddressToOffset(whitePix, &whiteTexOffset);

   // allocate quad batch buffer in vram
   for (int i = 0; i < FRAME_COUNT; ++i) {
      batchVerts[i] = (GfxVertex *)allocateVram(MAX_VERTS * sizeof(GfxVertex), 128);
      if (!batchVerts[i]) return -1;
      cellGcmAddressToOffset(&batchVerts[i][0].x, &batchOffset[i]);
      cellGcmAddressToOffset(&batchVerts[i][0].rgba, &batchColOffset[i]);
      cellGcmAddressToOffset(&batchVerts[i][0].u, &batchUvOffset[i]);
   }

   logInfo("[gfx] ready: %d x %d, pitch=%u, vram_used=%zu/%zu\n", screenW, screenH, framePitch, getUsedVram(), vramSize);
   return 0;
}

void termGfx(void)
{
   if (!initialized) {
      return;
   }
   // wait for last flip to complete
   cellGcmSetWaitFlip(CTX);
   cellGcmFinish(CTX, 1);

   // release all allocator metadata. The VRAM region itself is owned by libgcm;
   // dropping the block list (incl. the permanent frame/shader/batch blocks)
   // resets the allocator so a later initGfx() rebuilds it from scratch.
   VramBlock *block = blockListHead;
   while (block) {
      VramBlock *next = block->next;
      free(block);
      block = next;
   }
   blockListHead = NULL;
   vramAllocated = 0;

   initialized = 0;
}

void beginGfxFrame(void)
{
   if (!initialized) return;

   setRenderTarget();   // binds the display buffer + sets viewport/scissor to the screen

   cellGcmSetColorMask(CTX, CELL_GCM_COLOR_MASK_R | CELL_GCM_COLOR_MASK_G | CELL_GCM_COLOR_MASK_B | CELL_GCM_COLOR_MASK_A);
   cellGcmSetColorMaskMrt(CTX, 0);

   cellGcmSetDepthTestEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetFragmentProgramGammaEnable(CTX, CELL_GCM_FALSE);

   batchVertCount = 0;
   batchFlushStart = 0;
   frameDroppedCalls = 0;
   batchTexOffset = whiteTexOffset;
   batchTexW = 1;
   batchTexH = 1;
   batchTexPitch = 64;
   batchTexLinear = 0;
}

void clearGfx(uint32_t argb)
{
   if (!initialized) return;
   cellGcmSetScissor(CTX, 0, 0, targetW, targetH);
   cellGcmSetClearColor(CTX, argb);
   cellGcmSetClearSurface(CTX, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
}

static uint32_t argbToRgba(uint32_t argb)
{
   return ((argb & 0x00FF0000) << 8) | ((argb & 0x0000FF00) << 8) |
         ((argb & 0x000000FF) << 8) | ((argb >> 24) & 0xFF);
}

// convert a pixel coordinate to clip space [-1,1] within the current render target
static float toClipX(int px) { return (float)px / (float)targetW * 2.0f - 1.0f; }
static float toClipY(int py) { return 1.0f - (float)py / (float)targetH * 2.0f; }

// ensure we're batching against the 1x1 white texture (used by all color-only draws)
static void ensureWhiteTex(void)
{
   if (batchTexOffset != whiteTexOffset) {
      flushBatch();
      batchTexOffset = whiteTexOffset;
      batchTexW = 1;
      batchTexH = 1;
      batchTexPitch = 64;
      batchTexLinear = 0;
   }
}

int getGfxVertexBudget(void) { return MAX_VERTS; }
int getGfxVerticesUsed(void) { return batchVertCount; }

// batch is full for this frame: drop the call whole. counted, not logged here -
// the frame reports itself in endGfxFrame, so one overflowing screen cannot bury
// the log under a line per dropped call.
static int batchWouldOverflow(int vertsNeeded)
{
   if (batchVertCount + vertsNeeded <= MAX_VERTS) return 0;
   frameDroppedCalls++;
   return 1;
}

// a frame that is merely close to the cap is one screen element away from
// silently losing draws, so the warning comes while there is still headroom
// rather than after the picture has already broken. rate limited, because a
// heavy screen is heavy on every frame and the log is a file on disk.
#define BUDGET_WARN_PERCENT     80
#define BUDGET_WARN_INTERVAL_US 1000000

static void reportFrameBudget(void)
{
   int usedPercent = batchVertCount * 100 / MAX_VERTS;
   if (frameDroppedCalls == 0 && usedPercent < BUDGET_WARN_PERCENT) return;

   static uint64_t lastWarnUs;
   uint64_t now = sys_time_get_system_time();
   if (now - lastWarnUs < BUDGET_WARN_INTERVAL_US) return;
   lastWarnUs = now;

   if (frameDroppedCalls > 0)
      logWarn("[gfx] frame ran out of vertices (cap %d): %d draw calls dropped\n", MAX_VERTS, frameDroppedCalls);
   else
      logWarn("[gfx] frame used %d of %d vertices (%d%%)\n", batchVertCount, MAX_VERTS, usedPercent);
}

void fillGfxRectangle(int x, int y, int w, int h, uint32_t argb)
{
   if (!initialized) return;
   ensureWhiteTex();
   if (batchWouldOverflow(VERTS_PER_RECT)) return;

   if (x < 0) { w += x; x = 0; }
   if (y < 0) { h += y; y = 0; }
   if (x + w > targetW) w = targetW - x;
   if (y + h > targetH) h = targetH - y;
   if (w <= 0 || h <= 0) return;

   float x0 = toClipX(x),     y0 = toClipY(y);
   float x1 = toClipX(x + w), y1 = toClipY(y + h);
   uint32_t rgba = argbToRgba(argb);

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ x0, y0, 0.0f, rgba, 0.0f, 0.0f };
   v[1] = (GfxVertex){ x1, y0, 0.0f, rgba, 0.0f, 0.0f };
   v[2] = (GfxVertex){ x0, y1, 0.0f, rgba, 0.0f, 0.0f };
   v[3] = (GfxVertex){ x1, y0, 0.0f, rgba, 0.0f, 0.0f };
   v[4] = (GfxVertex){ x1, y1, 0.0f, rgba, 0.0f, 0.0f };
   v[5] = (GfxVertex){ x0, y1, 0.0f, rgba, 0.0f, 0.0f };

   batchVertCount += VERTS_PER_RECT;
}

// metro-style border: four edges drawn inward, so the outline stays within the w x h bounds.
void strokeGfxRectangle(int x, int y, int w, int h, int thickness, uint32_t argb)
{
   if (!initialized || thickness <= 0 || w <= 0 || h <= 0) return;
   if (batchWouldOverflow(VERTS_PER_STROKED_RECT)) return;   // all four edges or none
   if (thickness * 2 > w) thickness = w / 2;
   if (thickness * 2 > h) thickness = h / 2;

   fillGfxRectangle(x, y, w, thickness, argb);                              // top
   fillGfxRectangle(x, y + h - thickness, w, thickness, argb);              // bottom
   fillGfxRectangle(x, y + thickness, thickness, h - 2 * thickness, argb);  // left
   fillGfxRectangle(x + w - thickness, y + thickness, thickness, h - 2 * thickness, argb);  // right
}

// metro panel: filled interior with a border on top (the flat replacement for a rounded 9-slice sprite).
void drawGfxBox(int x, int y, int w, int h, int thickness, uint32_t fill, uint32_t border)
{
   if (batchWouldOverflow(VERTS_PER_RECT + VERTS_PER_STROKED_RECT)) return;   // fill and border together
   fillGfxRectangle(x, y, w, h, fill);
   strokeGfxRectangle(x, y, w, h, thickness, border);
}

// vertices are in pixel coords (origin top-left), converted to clip space internally.
void drawGfxTriangle(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2)
{
   if (!initialized) return;
   ensureWhiteTex();
   if (batchWouldOverflow(VERTS_PER_TRIANGLE)) return;

   float invW = 2.0f / (float)targetW;
   float invH = 2.0f / (float)targetH;
   float nx0 = x0 * invW - 1.0f, ny0 = 1.0f - y0 * invH;
   float nx1 = x1 * invW - 1.0f, ny1 = 1.0f - y1 * invH;
   float nx2 = x2 * invW - 1.0f, ny2 = 1.0f - y2 * invH;

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ nx0, ny0, 0.0f, argbToRgba(c0), 0.0f, 0.0f };
   v[1] = (GfxVertex){ nx1, ny1, 0.0f, argbToRgba(c1), 0.0f, 0.0f };
   v[2] = (GfxVertex){ nx2, ny2, 0.0f, argbToRgba(c2), 0.0f, 0.0f };
   batchVertCount += VERTS_PER_TRIANGLE;
}

// thick line via two triangles: builds a quad oriented along the line, expanded
// by thickness/2 in the perpendicular direction. degenerate (zero-length) lines
// are skipped.
void drawGfxLine(int x0, int y0, int x1, int y1, int thickness, uint32_t argb)
{
   if (!initialized || thickness <= 0) return;
   if (batchWouldOverflow(VERTS_PER_LINE)) return;   // both triangles or neither, so a line never tapers
   float dx = (float)(x1 - x0);
   float dy = (float)(y1 - y0);
   float len = sqrtf(dx * dx + dy * dy);
   if (len <= 0.0f) return;

   float t  = (float)thickness * 0.5f;
   float px = -dy / len * t;
   float py =  dx / len * t;

   float ax = (float)x0 + px, ay = (float)y0 + py;
   float bx = (float)x0 - px, by = (float)y0 - py;
   float cx = (float)x1 + px, cy = (float)y1 + py;
   float dx2 = (float)x1 - px, dy2 = (float)y1 - py;

   drawGfxTriangle(ax, ay, argb, bx, by, argb, cx, cy, argb);
   drawGfxTriangle(bx, by, argb, dx2, dy2, argb, cx, cy, argb);
}

// a circle is a fan of triangles, so its cost is its smoothness. segments finer
// than a pixel of outline are invisible and were the single largest waste in the
// tree (96 four-pixel bar caps at 24 segments each), so the count follows the
// radius: a 2-pixel dot gets 6, anything from 24 pixels up gets the full 24.
#define CIRCLE_MIN_SEGMENTS 6
#define CIRCLE_MAX_SEGMENTS 24

static int getCircleSegments(int radius)
{
   if (radius < CIRCLE_MIN_SEGMENTS) return CIRCLE_MIN_SEGMENTS;
   if (radius > CIRCLE_MAX_SEGMENTS) return CIRCLE_MAX_SEGMENTS;
   return radius;
}

void fillGfxCircle(int cx, int cy, int r, uint32_t argb)
{
   if (!initialized || r <= 0) return;
   ensureWhiteTex();
   int segments = getCircleSegments(r);
   if (batchWouldOverflow(segments * VERTS_PER_TRIANGLE)) return;

   uint32_t rgba = argbToRgba(argb);
   float fcx = toClipX(cx);
   float fcy = toClipY(cy);
   float step = 6.2831853f / (float)segments;
   float prevX = toClipX(cx + r);
   float prevY = fcy;

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   for (int i = 1; i <= segments; ++i) {
      float angle = step * (float)i;
      float nx = toClipX(cx + (int)(cosf(angle) * (float)r));
      float ny = toClipY(cy + (int)(sinf(angle) * (float)r));
      v[0] = (GfxVertex){ fcx, fcy, 0.0f, rgba, 0.0f, 0.0f };
      v[1] = (GfxVertex){ prevX, prevY, 0.0f, rgba, 0.0f, 0.0f };
      v[2] = (GfxVertex){ nx, ny, 0.0f, rgba, 0.0f, 0.0f };
      v += 3;
      prevX = nx;
      prevY = ny;
   }
   batchVertCount += segments * VERTS_PER_TRIANGLE;
}

static void flushBatch(void)
{
   int count = batchVertCount - batchFlushStart;
   if (count <= 0) return;

   cellGcmSetVertexProgram(CTX, shaderVp, shaderVpUcode);
   cellGcmSetFragmentProgram(CTX, shaderFp, shaderFpOffset);

   // point vertex arrays at the flush start offset
   uint32_t startByte = batchFlushStart * sizeof(GfxVertex);
   cellGcmSetVertexDataArray(CTX, shaderPosIdx, 0, sizeof(GfxVertex), 3, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderColIdx, 0, sizeof(GfxVertex), 4, CELL_GCM_VERTEX_UB, CELL_GCM_LOCATION_LOCAL, batchColOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderUvIdx, 0, sizeof(GfxVertex), 2, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchUvOffset[backBuffer] + startByte);

   // bind current batch texture
   CellGcmTexture tex;
   memset(&tex, 0, sizeof(tex));
   tex.format    = CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
   tex.mipmap    = 1;
   tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
   tex.cubemap   = CELL_GCM_FALSE;
   tex.remap     = CELL_GCM_REMAP_MODE(CELL_GCM_TEXTURE_REMAP_ORDER_XYXY, CELL_GCM_TEXTURE_REMAP_FROM_A, CELL_GCM_TEXTURE_REMAP_FROM_R, CELL_GCM_TEXTURE_REMAP_FROM_G, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP);
   tex.width     = batchTexW;
   tex.height    = batchTexH;
   tex.depth     = 1;
   tex.pitch     = batchTexPitch;
   tex.location  = CELL_GCM_LOCATION_LOCAL;
   tex.offset    = batchTexOffset;
   cellGcmSetTexture(CTX, shaderTexUnit, &tex);
   cellGcmSetTextureControl(CTX, shaderTexUnit, CELL_GCM_TRUE, 0, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
   cellGcmSetTextureAddress(CTX, shaderTexUnit, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
   int minFilt = batchTexLinear ? CELL_GCM_TEXTURE_LINEAR : CELL_GCM_TEXTURE_NEAREST;
   int magFilt = batchTexLinear ? CELL_GCM_TEXTURE_LINEAR : CELL_GCM_TEXTURE_NEAREST;
   cellGcmSetTextureFilter(CTX, shaderTexUnit, 0, minFilt, magFilt, CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);

   cellGcmSetInvalidateTextureCache(CTX, CELL_GCM_INVALIDATE_TEXTURE);

   cellGcmSetCullFaceEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetBlendEnable(CTX, CELL_GCM_TRUE);
   cellGcmSetBlendFunc(CTX, CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA, CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA);

   cellGcmSetDrawArrays(CTX, CELL_GCM_PRIMITIVE_TRIANGLES, 0, count);

   cellGcmSetBlendEnable(CTX, CELL_GCM_FALSE);

   batchFlushStart = batchVertCount;
}

void drawGfxTexture(int x, int y, int w, int h, GfxTexture tex, float u0, float v0, float u1, float v1, uint32_t tint, GfxFilter filter)
{
   if (!initialized || tex.w == 0 || tex.h == 0) return;

   // flush if texture or filter mode changes
   if (tex.offset != batchTexOffset || filter != batchTexLinear) {
      flushBatch();
      batchTexOffset = tex.offset;
      batchTexW = tex.w;
      batchTexH = tex.h;
      batchTexPitch = tex.pitch;
      batchTexLinear = filter;
   }

   if (batchWouldOverflow(VERTS_PER_RECT)) return;

   float cx0 = (float)x / (float)targetW * 2.0f - 1.0f;
   float cy0 = 1.0f - (float)y / (float)targetH * 2.0f;
   float cx1 = (float)(x + w) / (float)targetW * 2.0f - 1.0f;
   float cy1 = 1.0f - (float)(y + h) / (float)targetH * 2.0f;

   uint32_t rgba = argbToRgba(tint);

   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ cx0, cy0, 0.0f, rgba, u0, v0 };
   v[1] = (GfxVertex){ cx1, cy0, 0.0f, rgba, u1, v0 };
   v[2] = (GfxVertex){ cx0, cy1, 0.0f, rgba, u0, v1 };
   v[3] = (GfxVertex){ cx1, cy0, 0.0f, rgba, u1, v0 };
   v[4] = (GfxVertex){ cx1, cy1, 0.0f, rgba, u1, v1 };
   v[5] = (GfxVertex){ cx0, cy1, 0.0f, rgba, u0, v1 };

   batchVertCount += VERTS_PER_RECT;
}

// ============================================================================
// video frame path: YUV 4:2:0 planar frames decoded straight into RSX-visible
// main memory, drawn zero-copy with the yuv shader doing the color conversion
// ============================================================================

void *allocGfxVideoBuffer(size_t size)
{
   size_t mappedSize = (size + 0xFFFFF) & ~(size_t)0xFFFFF;   // MapMainMemory works in 1MB units
   void *buffer = memalign(1024 * 1024, mappedSize);
   if (!buffer) return 0;
   uint32_t ioOffset;
   if (cellGcmMapMainMemory(buffer, mappedSize, &ioOffset) != CELL_OK) {
      logError("[gfx] MapMainMemory %zu KB failed\n", mappedSize / 1024);
      free(buffer);
      return 0;
   }
   return buffer;
}

void freeGfxVideoBuffer(void *buffer)
{
   if (!buffer) return;
   cellGcmUnmapEaIoAddress(buffer);
   free(buffer);
}

// binds one Y/U/V plane as a single-channel linear texture the shader reads via .x
static void setYuvPlaneTexture(uint32_t unit, uint32_t offset, int width, int height)
{
   CellGcmTexture tex;
   memset(&tex, 0, sizeof(tex));
   tex.format    = CELL_GCM_TEXTURE_B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
   tex.mipmap    = 1;
   tex.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
   tex.cubemap   = CELL_GCM_FALSE;
   tex.remap     = CELL_GCM_REMAP_MODE(CELL_GCM_TEXTURE_REMAP_ORDER_XYXY, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_FROM_B, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP, CELL_GCM_TEXTURE_REMAP_REMAP);
   tex.width     = width;
   tex.height    = height;
   tex.depth     = 1;
   tex.pitch     = width;
   tex.location  = CELL_GCM_LOCATION_MAIN;
   tex.offset    = offset;
   cellGcmSetTexture(CTX, unit, &tex);
   cellGcmSetTextureControl(CTX, unit, CELL_GCM_TRUE, 0, 12 << 8, CELL_GCM_TEXTURE_MAX_ANISO_1);
   cellGcmSetTextureAddress(CTX, unit, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
   cellGcmSetTextureFilter(CTX, unit, 0, CELL_GCM_TEXTURE_LINEAR, CELL_GCM_TEXTURE_LINEAR, CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
}

void drawGfxYuvFrame(int x, int y, int w, int h, const void *yuvPlanes, int frameW, int frameH)
{
   if (!initialized || !yuvPlanes || frameW <= 0 || frameH <= 0) return;

   flushBatch();
   if (batchWouldOverflow(VERTS_PER_RECT)) return;

   uint32_t planeOffset;
   if (cellGcmAddressToOffset((void *)yuvPlanes, &planeOffset) != CELL_OK) return;

   // one opaque full-uv quad through the shared batch arrays
   float x0 = toClipX(x), y0 = toClipY(y), x1 = toClipX(x + w), y1 = toClipY(y + h);
   uint32_t white = 0xFFFFFFFFu;
   GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
   v[0] = (GfxVertex){ x0, y0, 0.0f, white, 0.0f, 0.0f };
   v[1] = (GfxVertex){ x1, y0, 0.0f, white, 1.0f, 0.0f };
   v[2] = (GfxVertex){ x0, y1, 0.0f, white, 0.0f, 1.0f };
   v[3] = (GfxVertex){ x1, y0, 0.0f, white, 1.0f, 0.0f };
   v[4] = (GfxVertex){ x1, y1, 0.0f, white, 1.0f, 1.0f };
   v[5] = (GfxVertex){ x0, y1, 0.0f, white, 0.0f, 1.0f };
   batchVertCount += VERTS_PER_RECT;

   cellGcmSetVertexProgram(CTX, shaderVp, shaderVpUcode);
   cellGcmSetFragmentProgram(CTX, shaderFpYuv, shaderFpYuvOffset);

   uint32_t startByte = batchFlushStart * sizeof(GfxVertex);
   cellGcmSetVertexDataArray(CTX, shaderPosIdx, 0, sizeof(GfxVertex), 3, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderColIdx, 0, sizeof(GfxVertex), 4, CELL_GCM_VERTEX_UB, CELL_GCM_LOCATION_LOCAL, batchColOffset[backBuffer] + startByte);
   cellGcmSetVertexDataArray(CTX, shaderUvIdx, 0, sizeof(GfxVertex), 2, CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, batchUvOffset[backBuffer] + startByte);

   uint32_t lumaBytes = (uint32_t)(frameW * frameH);
   setYuvPlaneTexture(shaderYuvTexUnit[0], planeOffset, frameW, frameH);
   setYuvPlaneTexture(shaderYuvTexUnit[1], planeOffset + lumaBytes, frameW / 2, frameH / 2);
   setYuvPlaneTexture(shaderYuvTexUnit[2], planeOffset + lumaBytes + lumaBytes / 4, frameW / 2, frameH / 2);

   cellGcmSetInvalidateTextureCache(CTX, CELL_GCM_INVALIDATE_TEXTURE);
   cellGcmSetCullFaceEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetBlendEnable(CTX, CELL_GCM_FALSE);
   cellGcmSetDrawArrays(CTX, CELL_GCM_PRIMITIVE_TRIANGLES, 0, 6);

   batchFlushStart = batchVertCount;
}

void setGfxVsync(GfxVsync vsync)
{
   cellGcmSetFlipMode(vsync == GFX_VSYNC_ON ? CELL_GCM_DISPLAY_VSYNC : CELL_GCM_DISPLAY_HSYNC);
}

void endGfxFrame(void)
{
   if (!initialized) return;

   flushBatch();
   reportFrameBudget();

   // queue this frame's flip without blocking the CPU on it. cellGcmSetWaitFlip is a gpu-side barrier:
   // the RSX won't draw into a buffer still on screen, so no tearing beyond what vsync-off already implies.
   if (cellGcmSetFlip(CTX, backBuffer) != CELL_OK) return;
   cellGcmFlush(CTX);
   cellGcmSetWaitFlip(CTX);
   flipsSubmitted++;

   backBuffer = (backBuffer + 1) % FRAME_COUNT;

   // stall only when the pipeline is full - one buffer on screen plus FRAME_COUNT-2 flips already queued.
   // with three buffers the CPU may run one flip ahead, so a slow GPU frame overlaps the next frame's work
   // instead of stalling us. the timeout is a safety net: a missed flip callback falls back to a brief wait
   // rather than a hard-lock.
   uint64_t startUs = sys_time_get_system_time();
   while ((uint32_t)(flipsSubmitted - flipsCompleted) >= FRAME_COUNT - 1) {
      if (sys_time_get_system_time() - startUs > 50000) break;   // 50ms guard, never a hard-lock
      sys_timer_usleep(200);
   }
   lastFlipWaitUs = sys_time_get_system_time() - startUs;
}

void finishGfx(void)
{
   if (!initialized) return;
   static uint32_t finishRef = 0;
   flushBatch();              // emit any pending quads first
   cellGcmFinish(CTX, ++finishRef);  // block until the RSX drains the command buffer
}

int getGfxScreenWidth(void)  { return screenW; }
int getGfxScreenHeight(void) { return screenH; }

int getGfxCoreClockMhz(void)
{
   CellGcmConfig config;
   cellGcmGetConfiguration(&config);
   return (int)(config.coreFrequency / 1000000);
}

int getGfxMemoryClockMhz(void)
{
   CellGcmConfig config;
   cellGcmGetConfiguration(&config);
   return (int)(config.memoryFrequency / 1000000);
}

size_t getUsedVram(void)
{
   return vramAllocated;
}

size_t getFreeVram(void)
{
   size_t total = 0;
   for (VramBlock *block = blockListHead; block; block = block->next)
      if (block->isFree) total += block->size;
   return total;
}

size_t getLargestFreeBlock(void)
{
   size_t largest = 0;
   for (VramBlock *block = blockListHead; block; block = block->next)
      if (block->isFree && block->size > largest) largest = block->size;
   return largest;
}

void freeGfxTexture(GfxTexture *tex)
{
   // offset 0 is frame buffer 0, never a texture, so it doubles as "empty".
   if (!tex || tex->offset == 0) return;
   freeVram((uint8_t *)vramBase + tex->offset);
   memset(tex, 0, sizeof(*tex));
}

// loadGfxTexture now lives in image-loader.c (generic PNG/JPEG); see image-loader.h.

// ---- offscreen render targets (render-to-texture) ----
// A target is a pitch-linear A8R8G8B8 surface in VRAM -- the SAME layout as a normal
// texture, so after rendering we can sample rt.tex directly through the usual draw path.

int createGfxRenderTarget(GfxRenderTarget *rt, int w, int h)
{
   if (!initialized || !rt || w <= 0 || h <= 0 || w > MAX_TEX_DIM || h > MAX_TEX_DIM) return -1;
   uint32_t pitch = ((uint32_t)(w * 4) + 63) & ~63;   // RSX color-surface pitch: 64B multiple
   uint32_t size  = pitch * (uint32_t)h;
   void *pixels = allocateVram(size, 128);            // 128B align (>= color-surface req)
   if (!pixels) { memset(rt, 0, sizeof(*rt)); return -1; }

   uint32_t offset;
   cellGcmAddressToOffset(pixels, &offset);

   memset(rt, 0, sizeof(*rt));
   rt->tex.offset = offset;
   rt->tex.w = w;
   rt->tex.h = h;
   rt->tex.pitch = (int)pitch;
   return 0;
}

void beginGfxRenderTarget(GfxRenderTarget *rt)
{
   if (!initialized || !rt || rt->tex.offset == 0) return;
   flushBatch();   // anything still queued belongs to the previous target
   setSurface(rt->tex.offset, (uint32_t)rt->tex.pitch, rt->tex.w, rt->tex.h);
}

// Returns a CPU-readable pointer to the FRONT (currently displayed) framebuffer in local memory --
// the last fully-rendered+flipped frame -- for screenshots. Writes the screen width/height and the
// row pitch in bytes; pixels are A8R8G8B8. Valid until the next flip; NULL if gfx isn't initialised.
// (On RPCS3 this needs the "Write Color Buffers" GPU option so rendered buffers are mirrored back to
// emulated memory; on real PS3 local memory is readable directly, like the debug-bridge does.)
const void *getGfxDisplayBuffer(int *w, int *h, int *pitch)
{
   if (!initialized) return (const void *)0;
   finishGfx();   // ensure the RSX has finished the frame that produced the front buffer
   uint32_t front = (backBuffer + FRAME_COUNT - 1) % FRAME_COUNT;   // the buffer flipped last
   if (w)     *w = screenW;
   if (h)     *h = screenH;
   if (pitch) *pitch = (int)framePitch;
   return (const uint8_t *)vramBase + frameOffset[front];
}

void endGfxRenderTarget(void)
{
   if (!initialized) return;
   flushBatch();        // commit the target's draws before we rebind the display buffer
   setRenderTarget();   // back to the display buffer + screen viewport/scissor
   // flushBatch() already issues SetInvalidateTextureCache before the next draw, so a
   // subsequent sample of this freshly rendered target re-reads it (RSX is in-order, so
   // the surface writes precede that read). Verify on HW when wiring transitions.
}

void freeGfxRenderTarget(GfxRenderTarget *rt)
{
   if (!rt) return;
   freeGfxTexture(&rt->tex);   // frees the VRAM and zeroes tex
}

uint32_t uploadGfxTexture(const void *rgba, int w, int h, int srcPitch)
{
   if (w <= 0 || h <= 0 || w > MAX_TEX_DIM || h > MAX_TEX_DIM) return 0;
   uint32_t alignedPitch = ((uint32_t)(w * 4) + 63) & ~63;
   uint32_t size = alignedPitch * (uint32_t)h;
   void *pixels = allocateVram(size, 64);
   if (!pixels) return 0;

   uint8_t *dst = (uint8_t *)pixels;
   const uint8_t *src = (const uint8_t *)rgba;
   for (int row = 0; row < h; ++row) {
      memcpy(dst + row * alignedPitch, src + row * srcPitch, (uint32_t)(w * 4));
   }

   uint32_t offset;
   cellGcmAddressToOffset(pixels, &offset);
   return offset;
}

void updateGfxTexture(uint32_t offset, const void *rgba, int w, int h, int srcPitch, int slotW, int slotH)
{
   uint32_t alignedPitch = ((uint32_t)(slotW * 4) + 63) & ~63;
   uint8_t *dst = (uint8_t *)vramBase + offset;
   const uint8_t *src = (const uint8_t *)rgba;
   uint32_t rowBytes = (uint32_t)(w * 4);

   // copy content rows, clearing any right-margin padding
   for (int row = 0; row < h; ++row) {
      uint8_t *rowDst = dst + row * alignedPitch;
      memcpy(rowDst, src + row * srcPitch, rowBytes);
      if (rowBytes < alignedPitch)
         memset(rowDst + rowBytes, 0, alignedPitch - rowBytes);
   }

   // clear any bottom rows beyond the new content
   if (h < slotH) {
      memset(dst + h * alignedPitch, 0, alignedPitch * (uint32_t)(slotH - h));
   }
}
