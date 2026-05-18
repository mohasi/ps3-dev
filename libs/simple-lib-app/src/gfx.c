// 2D graphics implementation using libgcm/RSX.
// Double-buffered, uncapped. Shader-based batched quad renderer.
#include "gfx.h"
#include "colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <sys/timer.h>
#include <cell/gcm.h>
#include <cell/codec/pngdec.h>
#include <cell/sysmodule.h>
#include <sysutil/sysutil_sysparam.h>

#define CTX gCellGcmCurrentContext

#define FRAME_COUNT       2
#define HOST_MEM_SIZE     (1 * 1024 * 1024)
#define CMD_BUFFER_SIZE   (0x10000)
#define BYTES_PER_PIXEL   4

// max quads per frame (6 verts each)
#define MAX_QUADS         4096
#define MAX_VERTS         (MAX_QUADS * 6)

typedef struct {
	float x, y, z;
	uint32_t rgba;
	float u, v;
} GfxVertex;

static int       initialized = 0;
static int       screenW = 0;
static int       screenH = 0;
static uint32_t  framePitch = 0;
static uint32_t  frameOffset[FRAME_COUNT];
static uint32_t  backBuffer = 0;

static uint8_t  *vramBase = 0;
static size_t    vramSize = 0;
static size_t    vramUsed = 0;

// double-buffered vertex batch (write one while GPU reads the other)
static GfxVertex *batchVerts[FRAME_COUNT];
static uint32_t   batchOffset[FRAME_COUNT];
static uint32_t   batchColOffset[FRAME_COUNT];
static uint32_t   batchUvOffset[FRAME_COUNT];
static int        batchVertCount = 0;
static int        batchFlushStart = 0;

// shader state
extern struct _CGprogram _binary_vpshader_vpo_start;
extern struct _CGprogram _binary_fpshader_fpo_start;

static CGprogram shaderVp;
static CGprogram shaderFp;
static void     *shaderVpUcode;
static void     *shaderFpUcode;
static uint32_t  shaderFpOffset;
static uint32_t  shaderPosIdx;
static uint32_t  shaderColIdx;
static uint32_t  shaderUvIdx;
static uint32_t  shaderTexUnit;

// 1x1 white texture for color-only draws
static uint32_t whiteTexOffset = 0;

// current batch texture state
static uint32_t batchTexOffset = 0;
static int      batchTexW = 1;
static int      batchTexH = 1;
static int      batchTexPitch = 64;
static int      batchTexLinear = 0;

static void flushBatch(void);

void *vramAlloc(size_t size, size_t alignment)
{
	size_t base = (size_t)vramBase + vramUsed;
	size_t aligned = (base + alignment - 1) & ~(alignment - 1);
	size_t newUsed = (aligned - (size_t)vramBase) + size;

	if (newUsed > vramSize) {
		printf("[gfx] vramAlloc: out of VRAM\n");
		return 0;
	}
	vramUsed = newUsed;
	return (void *)aligned;
}

static void waitFlip(void)
{
	while (cellGcmGetFlipStatus() != 0) {
		sys_timer_usleep(300);
	}
}

static void setRenderTarget(void)
{
	CellGcmSurface sf;
	memset(&sf, 0, sizeof(sf));

	sf.colorFormat      = CELL_GCM_SURFACE_A8R8G8B8;
	sf.colorTarget      = CELL_GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = CELL_GCM_LOCATION_LOCAL;
	sf.colorOffset[0]   = frameOffset[backBuffer];
	sf.colorPitch[0]    = framePitch;

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
	sf.width     = screenW;
	sf.height    = screenH;
	sf.x         = 0;
	sf.y         = 0;

	cellGcmSetSurface(CTX, &sf);
}

int initGfx(GfxVsync vsync)
{
	int ret;

	if (initialized) {
		return 0;
	}

	cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);

	// allocate host-side command buffer pool
	void *hostAddr = memalign(1024 * 1024, HOST_MEM_SIZE);
	if (!hostAddr) {
		printf("[gfx] memalign host failed\n");
		return -1;
	}

	ret = cellGcmInit(CMD_BUFFER_SIZE, HOST_MEM_SIZE, hostAddr);
	if (ret < 0) {
		printf("[gfx] cellGcmInit failed: 0x%08x\n", ret);
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
		printf("[gfx] cellVideoOutConfigure: 0x%08x\n", ret);
		return ret;
	}

	cellGcmSetFlipMode(vsync == GFX_VSYNC_ON ? CELL_GCM_DISPLAY_VSYNC : CELL_GCM_DISPLAY_HSYNC);

	// set up vram allocator
	CellGcmConfig config;
	cellGcmGetConfiguration(&config);
	vramBase = (uint8_t *)config.localAddress;
	vramSize = (size_t)config.localSize;
	vramUsed = 0;

	// allocate frame buffers in vram
	uint32_t bufferHeight = cellGcmAlign(CELL_GCM_ZCULL_ALIGN_HEIGHT, screenH);
	uint32_t colorSize    = framePitch * bufferHeight;

	for (int i = 0; i < FRAME_COUNT; ++i) {
		void *addr = vramAlloc(colorSize, CELL_GCM_TILE_ALIGN_OFFSET);
		if (!addr) {
			return -1;
		}

		ret = cellGcmAddressToOffset(addr, &frameOffset[i]);
		if (ret < 0) {
			printf("[gfx] cellGcmAddressToOffset[%d]: 0x%08x\n", i, ret);
			return ret;
		}

		cellGcmSetTileInfo(i, CELL_GCM_LOCATION_LOCAL, frameOffset[i], colorSize, framePitch, CELL_GCM_COMPMODE_DISABLED, 0, 0);
		cellGcmBindTile(i);

		ret = cellGcmSetDisplayBuffer(i, frameOffset[i], framePitch, screenW, screenH);
		if (ret < 0) {
			printf("[gfx] cellGcmSetDisplayBuffer[%d]: 0x%08x\n", i, ret);
			return ret;
		}
	}

	backBuffer = 0;
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

	shaderFpUcode = vramAlloc(fpSize, 64);
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

	// 1x1 white texture (64-byte pitch minimum for RSX)
	uint32_t *whitePix = (uint32_t *)vramAlloc(64, 64);
	if (!whitePix) return -1;
	*whitePix = COLOR_WHITE;
	cellGcmAddressToOffset(whitePix, &whiteTexOffset);

	// allocate quad batch buffer in vram
	for (int i = 0; i < FRAME_COUNT; ++i) {
		batchVerts[i] = (GfxVertex *)vramAlloc(MAX_VERTS * sizeof(GfxVertex), 128);
		if (!batchVerts[i]) return -1;
		cellGcmAddressToOffset(&batchVerts[i][0].x, &batchOffset[i]);
		cellGcmAddressToOffset(&batchVerts[i][0].rgba, &batchColOffset[i]);
		cellGcmAddressToOffset(&batchVerts[i][0].u, &batchUvOffset[i]);
	}

	printf("[gfx] ready: %d x %d, pitch=%u, vram_used=%zu/%zu\n", screenW, screenH, framePitch, vramUsed, vramSize);
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
	initialized = 0;
}

void beginGfxFrame(void)
{
	if (!initialized) return;

	setRenderTarget();

	cellGcmSetColorMask(CTX, CELL_GCM_COLOR_MASK_R | CELL_GCM_COLOR_MASK_G | CELL_GCM_COLOR_MASK_B | CELL_GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(CTX, 0);

	float scale[4]  = { screenW * 0.5f,  screenH * -0.5f, 0.5f, 0.0f };
	float offset[4] = { screenW * 0.5f,  screenH *  0.5f, 0.5f, 0.0f };
	cellGcmSetViewport(CTX, 0, 0, screenW, screenH, 0.0f, 1.0f, scale, offset);

	cellGcmSetScissor(CTX, 0, 0, screenW, screenH);
	cellGcmSetDepthTestEnable(CTX, CELL_GCM_FALSE);
	cellGcmSetFragmentProgramGammaEnable(CTX, CELL_GCM_TRUE);

	batchVertCount = 0;
	batchFlushStart = 0;
	batchTexOffset = whiteTexOffset;
	batchTexW = 1;
	batchTexH = 1;
	batchTexPitch = 64;
	batchTexLinear = 0;
}

void clearGfx(uint32_t argb)
{
	if (!initialized) return;
	cellGcmSetScissor(CTX, 0, 0, screenW, screenH);
	cellGcmSetClearColor(CTX, argb);
	cellGcmSetClearSurface(CTX, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B | CELL_GCM_CLEAR_A);
}

static uint32_t argbToRgba(uint32_t argb)
{
	return ((argb & 0x00FF0000) << 8) | ((argb & 0x0000FF00) << 8) |
		   ((argb & 0x000000FF) << 8) | ((argb >> 24) & 0xFF);
}

// convert a pixel coordinate to clip space [-1,1]
static float toClipX(int px) { return (float)px / (float)screenW * 2.0f - 1.0f; }
static float toClipY(int py) { return 1.0f - (float)py / (float)screenH * 2.0f; }

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

void fillGfxRectangle(int x, int y, int w, int h, uint32_t argb)
{
	if (!initialized) return;
	ensureWhiteTex();
	if (batchVertCount + 6 > MAX_VERTS) return;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > screenW)  w = screenW  - x;
	if (y + h > screenH) h = screenH - y;
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

	batchVertCount += 6;
}

// vertices are in pixel coords (origin top-left), converted to clip space internally.
void drawGfxTriangle(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2)
{
	if (!initialized) return;
	ensureWhiteTex();
	if (batchVertCount + 3 > MAX_VERTS) return;

	float invW = 2.0f / (float)screenW;
	float invH = 2.0f / (float)screenH;
	float nx0 = x0 * invW - 1.0f, ny0 = 1.0f - y0 * invH;
	float nx1 = x1 * invW - 1.0f, ny1 = 1.0f - y1 * invH;
	float nx2 = x2 * invW - 1.0f, ny2 = 1.0f - y2 * invH;

	GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
	v[0] = (GfxVertex){ nx0, ny0, 0.0f, argbToRgba(c0), 0.0f, 0.0f };
	v[1] = (GfxVertex){ nx1, ny1, 0.0f, argbToRgba(c1), 0.0f, 0.0f };
	v[2] = (GfxVertex){ nx2, ny2, 0.0f, argbToRgba(c2), 0.0f, 0.0f };
	batchVertCount += 3;
}

// thick line via two triangles: builds a quad oriented along the line, expanded
// by thickness/2 in the perpendicular direction. degenerate (zero-length) lines
// are skipped.
void drawGfxLine(int x0, int y0, int x1, int y1, int thickness, uint32_t argb)
{
	if (!initialized || thickness <= 0) return;
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

#define CIRCLE_SEGMENTS 24

void fillGfxCircle(int cx, int cy, int r, uint32_t argb)
{
	if (!initialized || r <= 0) return;
	ensureWhiteTex();
	if (batchVertCount + CIRCLE_SEGMENTS * 3 > MAX_VERTS) return;

	uint32_t rgba = argbToRgba(argb);
	float fcx = toClipX(cx);
	float fcy = toClipY(cy);
	float step = 6.2831853f / (float)CIRCLE_SEGMENTS;
	float prevX = toClipX(cx + r);
	float prevY = fcy;

	GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
	for (int i = 1; i <= CIRCLE_SEGMENTS; ++i) {
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
	batchVertCount += CIRCLE_SEGMENTS * 3;
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

	if (batchVertCount + 6 > MAX_VERTS) return;

	float cx0 = (float)x / (float)screenW * 2.0f - 1.0f;
	float cy0 = 1.0f - (float)y / (float)screenH * 2.0f;
	float cx1 = (float)(x + w) / (float)screenW * 2.0f - 1.0f;
	float cy1 = 1.0f - (float)(y + h) / (float)screenH * 2.0f;

	uint32_t rgba = argbToRgba(tint);

	GfxVertex *v = &batchVerts[backBuffer][batchVertCount];
	v[0] = (GfxVertex){ cx0, cy0, 0.0f, rgba, u0, v0 };
	v[1] = (GfxVertex){ cx1, cy0, 0.0f, rgba, u1, v0 };
	v[2] = (GfxVertex){ cx0, cy1, 0.0f, rgba, u0, v1 };
	v[3] = (GfxVertex){ cx1, cy0, 0.0f, rgba, u1, v0 };
	v[4] = (GfxVertex){ cx1, cy1, 0.0f, rgba, u1, v1 };
	v[5] = (GfxVertex){ cx0, cy1, 0.0f, rgba, u0, v1 };

	batchVertCount += 6;
}

void endGfxFrame(void)
{
	if (!initialized) return;

	flushBatch();

	waitFlip();
	cellGcmResetFlipStatus();

	if (cellGcmSetFlip(CTX, backBuffer) != CELL_OK) {
		return;
	}
	cellGcmFlush(CTX);
	cellGcmSetWaitFlip(CTX);

	backBuffer = (backBuffer + 1) % FRAME_COUNT;
}

int getGfxScreenWidth(void)  { return screenW; }
int getGfxScreenHeight(void) { return screenH; }

void resetGfxVram(size_t mark)
{
	vramUsed = mark;
}

size_t getUsedGfxVram(void)
{
	return vramUsed;
}

// pngdec callbacks
static void *pngMalloc(uint32_t size, void *arg)
{
	(void)arg;
	return malloc(size);
}

static int32_t pngFree(void *ptr, void *arg)
{
	(void)arg;
	free(ptr);
	return 0;
}

GfxTexture loadGfxTexture(const char *path)
{
	GfxTexture result = { 0, 0, 0 };
	CellPngDecMainHandle mainHandle;
	CellPngDecSubHandle subHandle;
	CellPngDecThreadInParam threadIn;
	CellPngDecThreadOutParam threadOut;
	CellPngDecSrc src;
	CellPngDecOpnInfo openInfo;
	CellPngDecInfo info;
	CellPngDecInParam inParam;
	CellPngDecOutParam outParam;
	CellPngDecDataOutInfo dataInfo;
	CellPngDecDataCtrlParam ctrl;
	int ret;

	// create decoder
	threadIn.spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;
	threadIn.ppuThreadPriority = 512;
	threadIn.spuThreadPriority = 200;
	threadIn.cbCtrlMallocFunc = pngMalloc;
	threadIn.cbCtrlMallocArg = NULL;
	threadIn.cbCtrlFreeFunc = pngFree;
	threadIn.cbCtrlFreeArg = NULL;

	ret = cellPngDecCreate(&mainHandle, &threadIn, &threadOut);
	if (ret != CELL_OK) return result;

	// open file
	src.srcSelect = CELL_PNGDEC_FILE;
	src.fileName = path;
	src.fileOffset = 0;
	src.fileSize = 0;
	src.streamPtr = NULL;
	src.streamSize = 0;
	src.spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;

	ret = cellPngDecOpen(mainHandle, &subHandle, &src, &openInfo);
	if (ret != CELL_OK) { cellPngDecDestroy(mainHandle); return result; }

	// read header
	ret = cellPngDecReadHeader(mainHandle, subHandle, &info);
	if (ret != CELL_OK) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return result; }

	// set decode params -- output as ARGB, 8-bit
	int hasAlpha = (info.colorSpace == CELL_PNGDEC_RGBA || info.colorSpace == CELL_PNGDEC_GRAYSCALE_ALPHA);
	inParam.commandPtr = NULL;
	inParam.outputMode = CELL_PNGDEC_TOP_TO_BOTTOM;
	inParam.outputColorSpace = CELL_PNGDEC_ARGB;
	inParam.outputBitDepth = 8;
	inParam.outputPackFlag = CELL_PNGDEC_1BYTE_PER_1PIXEL;
	inParam.outputAlphaSelect = hasAlpha ? CELL_PNGDEC_STREAM_ALPHA : CELL_PNGDEC_FIX_ALPHA;
	inParam.outputColorAlpha = 0xff;

	ret = cellPngDecSetParameter(mainHandle, subHandle, &inParam, &outParam);
	if (ret != CELL_OK) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return result; }

	uint32_t w = outParam.outputWidth;
	uint32_t h = outParam.outputHeight;
	uint32_t stride = outParam.outputWidthByte;

	// decode into heap (pngdec can't write directly to RSX local mem)
	void *tempBuf = malloc(stride * h);
	if (!tempBuf) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return result; }

	ctrl.outputBytesPerLine = stride;
	ret = cellPngDecDecodeData(mainHandle, subHandle, (uint8_t *)tempBuf, &ctrl, &dataInfo);
	cellPngDecClose(mainHandle, subHandle);
	cellPngDecDestroy(mainHandle);

	if (ret != CELL_OK || dataInfo.status != CELL_PNGDEC_DEC_STATUS_FINISH) { free(tempBuf); return result; }

	// copy to VRAM with 64-byte aligned pitch
	uint32_t alignedPitch = (stride + 63) & ~63;
	uint32_t size = alignedPitch * h;
	void *pixels = vramAlloc(size, 64);
	if (!pixels) { free(tempBuf); return result; }

	uint8_t *dst = (uint8_t *)pixels;
	uint8_t *s = (uint8_t *)tempBuf;
	for (uint32_t row = 0; row < h; ++row) {
		memcpy(dst + row * alignedPitch, s + row * stride, stride);
	}
	free(tempBuf);

	cellGcmAddressToOffset(pixels, &result.offset);
	result.w = (int)w;
	result.h = (int)h;
	result.pitch = (int)alignedPitch;
	return result;
}

uint32_t uploadGfxTexture(const void *rgba, int w, int h, int srcPitch)
{
	uint32_t alignedPitch = ((uint32_t)(w * 4) + 63) & ~63;
	uint32_t size = alignedPitch * (uint32_t)h;
	void *pixels = vramAlloc(size, 64);
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
