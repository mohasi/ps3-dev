// image-loader - PNG/JPEG decode for the image viewer.
//
// Two layers:
//   * decode-to-buffer: reads a file into a heap ARGB8888 buffer. No RSX/VRAM
//     calls, so it is safe to run on a background worker thread.
//   * upload: turns a decoded buffer into a VRAM GfxTexture. Touches the RSX
//     bump allocator, so it must run on the main thread only.
//
// requestImageAsync()/pollImageAsync() drive the background worker so a large
// image never freezes the UI; the caller uploads the result on the main thread.
#include "image-loader.h"
#include "dir-playlist.h"       // listDirFiltered (shared folder scan + sort)
#include "file.h"               // getExtension
#include "string-utilities.h"   // strCmpICase, strCopy
#include "dbg.h"                // logError
#include "thread.h"             // spawnThread, lwmutex helpers, sleepMs

#include <stdlib.h>
#include <string.h>
#include <cell/codec/pngdec.h>
#include <cell/codec/jpgdec.h>

// The RSX cannot sample a 2D texture larger than 4096x4096 -- handing it an
// oversized texture corrupts the display and hangs the GPU. So we never produce
// a buffer bigger than this: JPEG is downscaled by the codec, PNG (no codec
// downscale) is rejected when it exceeds the limit.
#define MAX_TEX_DIM 4096

// ---------------------------------------------------------------------------
// format check + directory listing
// ---------------------------------------------------------------------------

int isSupportedImageFormat(const char *filename)
{
   const char *ext = getExtension(filename);
   if (!ext) return 0;
   return strCmpICase(ext, "png")  == 0 ||
         strCmpICase(ext, "jpg")  == 0 ||
         strCmpICase(ext, "jpeg") == 0;
}

int listSupportedImages(const char *dir, char names[][IMAGE_NAME_MAX], int maxCount)
{
   // IMAGE_NAME_MAX and DIR_PLAYLIST_NAME_MAX are both 256, so the array types match.
   return listDirFiltered(dir, names, maxCount, isSupportedImageFormat);
}

// ---------------------------------------------------------------------------
// decode to heap buffer (no VRAM; safe off the main thread)
// ---------------------------------------------------------------------------

// codec control-memory callbacks (heap).
static void *imgMalloc(uint32_t size, void *arg) { (void)arg; return malloc(size); }
static int32_t imgFree(void *ptr, void *arg)     { (void)arg; free(ptr); return 0; }

// Fill a PNG source descriptor for a file or an in-memory buffer.
static void pngSrcFile(CellPngDecSrc *s, const char *path)
{
   s->srcSelect = CELL_PNGDEC_FILE; s->fileName = path;
   s->fileOffset = 0; s->fileSize = 0; s->streamPtr = NULL; s->streamSize = 0;
   s->spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;
}
static void pngSrcMem(CellPngDecSrc *s, const void *buf, uint32_t size)
{
   s->srcSelect = CELL_PNGDEC_BUFFER; s->fileName = NULL;
   s->fileOffset = 0; s->fileSize = 0; s->streamPtr = (void *)buf; s->streamSize = size;
   s->spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE;
}

// decodes a PNG from a prepared source into a freshly malloc'd ARGB8888 buffer. returns 0 ok.
static int decodePng(const CellPngDecSrc *src, ImageBuffer *out)
{
   CellPngDecMainHandle mainHandle;
   CellPngDecSubHandle  subHandle;
   CellPngDecThreadInParam  threadIn;
   CellPngDecThreadOutParam threadOut;
   CellPngDecOpnInfo    openInfo;
   CellPngDecInfo       info;
   CellPngDecInParam    inParam;
   CellPngDecOutParam   outParam;
   CellPngDecDataOutInfo dataInfo;
   CellPngDecDataCtrlParam ctrl;
   int ret;

   threadIn.spuThreadEnable   = CELL_PNGDEC_SPU_THREAD_DISABLE;
   threadIn.ppuThreadPriority = 512;
   threadIn.spuThreadPriority = 200;
   threadIn.cbCtrlMallocFunc  = imgMalloc;
   threadIn.cbCtrlMallocArg   = NULL;
   threadIn.cbCtrlFreeFunc    = imgFree;
   threadIn.cbCtrlFreeArg     = NULL;

   ret = cellPngDecCreate(&mainHandle, &threadIn, &threadOut);
   if (ret != CELL_OK) return -1;

   ret = cellPngDecOpen(mainHandle, &subHandle, src, &openInfo);
   if (ret != CELL_OK) { cellPngDecDestroy(mainHandle); return -1; }

   ret = cellPngDecReadHeader(mainHandle, subHandle, &info);
   if (ret != CELL_OK) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return -1; }

   // The PNG codec has no downscale, so an oversized PNG can't be made into a
   // legal texture -- fail cleanly rather than hand the RSX something it hangs on.
   if (info.imageWidth > MAX_TEX_DIM || info.imageHeight > MAX_TEX_DIM) {
      logError("[image-loader] PNG too large for RSX: %ux%u (max %d)\n",
               info.imageWidth, info.imageHeight, MAX_TEX_DIM);
      cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle);
      return -1;
   }

   int hasAlpha = (info.colorSpace == CELL_PNGDEC_RGBA || info.colorSpace == CELL_PNGDEC_GRAYSCALE_ALPHA);
   inParam.commandPtr       = NULL;
   inParam.outputMode       = CELL_PNGDEC_TOP_TO_BOTTOM;
   inParam.outputColorSpace = CELL_PNGDEC_ARGB;
   inParam.outputBitDepth   = 8;
   inParam.outputPackFlag   = CELL_PNGDEC_1BYTE_PER_1PIXEL;
   inParam.outputAlphaSelect = hasAlpha ? CELL_PNGDEC_STREAM_ALPHA : CELL_PNGDEC_FIX_ALPHA;
   inParam.outputColorAlpha = 0xff;

   ret = cellPngDecSetParameter(mainHandle, subHandle, &inParam, &outParam);
   if (ret != CELL_OK) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return -1; }

   uint32_t w = outParam.outputWidth;
   uint32_t h = outParam.outputHeight;
   uint32_t stride = outParam.outputWidthByte;

   void *buf = malloc(stride * h);
   if (!buf) { cellPngDecClose(mainHandle, subHandle); cellPngDecDestroy(mainHandle); return -1; }

   ctrl.outputBytesPerLine = stride;
   ret = cellPngDecDecodeData(mainHandle, subHandle, (uint8_t *)buf, &ctrl, &dataInfo);
   cellPngDecClose(mainHandle, subHandle);
   cellPngDecDestroy(mainHandle);

   if (ret != CELL_OK || dataInfo.status != CELL_PNGDEC_DEC_STATUS_FINISH) { free(buf); return -1; }

   out->pixels = buf;
   out->w = (int)w;
   out->h = (int)h;
   out->pitch = (int)stride;
   return 0;
}

// Fill a JPEG source descriptor for a file or an in-memory buffer.
static void jpgSrcFile(CellJpgDecSrc *s, const char *path)
{
   s->srcSelect = CELL_JPGDEC_FILE; s->fileName = path;
   s->fileOffset = 0; s->fileSize = 0; s->streamPtr = NULL; s->streamSize = 0;
   s->spuThreadEnable = CELL_JPGDEC_SPU_THREAD_DISABLE;
}
static void jpgSrcMem(CellJpgDecSrc *s, const void *buf, uint32_t size)
{
   s->srcSelect = CELL_JPGDEC_BUFFER; s->fileName = NULL;
   s->fileOffset = 0; s->fileSize = 0; s->streamPtr = (void *)buf; s->streamSize = size;
   s->spuThreadEnable = CELL_JPGDEC_SPU_THREAD_DISABLE;
}

// decodes a JPEG from a prepared source into a freshly malloc'd ARGB8888 buffer. returns 0 ok.
static int decodeJpeg(const CellJpgDecSrc *src, ImageBuffer *out)
{
   CellJpgDecMainHandle mainHandle;
   CellJpgDecSubHandle  subHandle;
   CellJpgDecThreadInParam  threadIn;
   CellJpgDecThreadOutParam threadOut;
   CellJpgDecOpnInfo    openInfo;
   CellJpgDecInfo       info;
   CellJpgDecInParam    inParam;
   CellJpgDecOutParam   outParam;
   CellJpgDecDataCtrlParam ctrl;
   CellJpgDecDataOutInfo   dataInfo;
   int ret;

   threadIn.spuThreadEnable   = CELL_JPGDEC_SPU_THREAD_DISABLE;
   threadIn.ppuThreadPriority = 512;
   threadIn.spuThreadPriority = 200;
   threadIn.cbCtrlMallocFunc  = imgMalloc;
   threadIn.cbCtrlMallocArg   = NULL;
   threadIn.cbCtrlFreeFunc    = imgFree;
   threadIn.cbCtrlFreeArg     = NULL;

   ret = cellJpgDecCreate(&mainHandle, &threadIn, &threadOut);
   if (ret != CELL_OK) return -1;

   ret = cellJpgDecOpen(mainHandle, &subHandle, src, &openInfo);
   if (ret != CELL_OK) { cellJpgDecDestroy(mainHandle); return -1; }

   ret = cellJpgDecReadHeader(mainHandle, subHandle, &info);
   if (ret != CELL_OK) { cellJpgDecClose(mainHandle, subHandle); cellJpgDecDestroy(mainHandle); return -1; }

   // choose the smallest codec downscale (1/2/4/8) that brings both dimensions
   // within the RSX texture limit, so a big camera JPEG decodes to a legal size.
   uint32_t downScale = 1;
   while (downScale < 8 &&
          ((info.imageWidth  + downScale - 1) / downScale > MAX_TEX_DIM ||
           (info.imageHeight + downScale - 1) / downScale > MAX_TEX_DIM))
      downScale <<= 1;

   // output ARGB8888. zero first so reserved[] is clean.
   memset(&inParam, 0, sizeof(inParam));
   inParam.commandPtr       = NULL;
   inParam.downScale        = downScale;
   inParam.method           = CELL_JPGDEC_QUALITY;
   inParam.outputMode       = CELL_JPGDEC_TOP_TO_BOTTOM;
   inParam.outputColorSpace = CELL_JPG_ARGB;
   inParam.outputColorAlpha = 0xff;  // JPEG has no alpha; force opaque

   ret = cellJpgDecSetParameter(mainHandle, subHandle, &inParam, &outParam);
   if (ret != CELL_OK) { cellJpgDecClose(mainHandle, subHandle); cellJpgDecDestroy(mainHandle); return -1; }

   uint32_t w = outParam.outputWidth;
   uint32_t h = outParam.outputHeight;
   uint32_t stride = (uint32_t)outParam.outputWidthByte;

   // guard: even at 1/8 an enormous image could exceed the limit.
   if (w > MAX_TEX_DIM || h > MAX_TEX_DIM) {
      logError("[image-loader] JPEG too large for RSX: %ux%u (max %d)\n", w, h, MAX_TEX_DIM);
      cellJpgDecClose(mainHandle, subHandle); cellJpgDecDestroy(mainHandle);
      return -1;
   }

   void *buf = malloc(stride * h);
   if (!buf) { cellJpgDecClose(mainHandle, subHandle); cellJpgDecDestroy(mainHandle); return -1; }

   ctrl.outputBytesPerLine = stride;
   ret = cellJpgDecDecodeData(mainHandle, subHandle, (uint8_t *)buf, &ctrl, &dataInfo);
   cellJpgDecClose(mainHandle, subHandle);
   cellJpgDecDestroy(mainHandle);

   if (ret != CELL_OK || dataInfo.status != CELL_JPGDEC_DEC_STATUS_FINISH) { free(buf); return -1; }

   out->pixels = buf;
   out->w = (int)w;
   out->h = (int)h;
   out->pitch = (int)stride;
   return 0;
}

// dispatches a file by extension. returns 0 on success, -1 otherwise.
static int decodeImageToBuffer(const char *path, ImageBuffer *out)
{
   const char *ext = getExtension(path);
   if (!ext) { logError("[image-loader] no extension: %s\n", path); return -1; }

   if (strCmpICase(ext, "png") == 0)  { CellPngDecSrc s; pngSrcFile(&s, path); return decodePng(&s, out); }
   if (strCmpICase(ext, "jpg") == 0 || strCmpICase(ext, "jpeg") == 0)
      { CellJpgDecSrc s; jpgSrcFile(&s, path); return decodeJpeg(&s, out); }

   logError("[image-loader] unsupported format: %s\n", path);
   return -1;
}

// dispatches an in-memory image by sniffing its magic bytes (PNG \x89PNG, JPEG \xFF\xD8).
// returns 0 on success, -1 otherwise. No temp file / no filesystem touch.
static int decodeImageMem(const void *data, uint32_t size, ImageBuffer *out)
{
   const unsigned char *b = (const unsigned char *)data;
   if (size >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
      { CellPngDecSrc s; pngSrcMem(&s, data, size); return decodePng(&s, out); }
   if (size >= 2 && b[0] == 0xFF && b[1] == 0xD8)
      { CellJpgDecSrc s; jpgSrcMem(&s, data, size); return decodeJpeg(&s, out); }

   logError("[image-loader] unrecognised image header in memory buffer (%u bytes)\n", size);
   return -1;
}

// ---------------------------------------------------------------------------
// VRAM upload (main thread only)
// ---------------------------------------------------------------------------

GfxTexture uploadImageBuffer(const ImageBuffer *buf)
{
   GfxTexture zero = { 0, 0, 0, 0 };
   if (!buf->pixels) return zero;

   // never hand the RSX a texture beyond its limit (decode should already cap,
   // this is the last line of defence against a GPU-hanging upload).
   if (buf->w <= 0 || buf->h <= 0 || buf->w > MAX_TEX_DIM || buf->h > MAX_TEX_DIM) {
      logError("[image-loader] refusing oversized texture %dx%d\n", buf->w, buf->h);
      return zero;
   }

   uint32_t offset = uploadGfxTexture(buf->pixels, buf->w, buf->h, buf->pitch);
   if (offset == 0) return zero;

   GfxTexture t;
   t.offset = offset;
   t.w = buf->w;
   t.h = buf->h;
   t.pitch = (int)(((uint32_t)(buf->w * 4) + 63) & ~63u);  // matches uploadGfxTexture
   return t;
}

void freeImageBuffer(ImageBuffer *buf)
{
   if (buf->pixels) { free(buf->pixels); buf->pixels = NULL; }
}

// Synchronous load of a PNG or JPEG file into a VRAM texture (main thread).
// Dispatches by extension via the shared decoder (replaces the old PNG-only loader).
GfxTexture loadGfxTexture(const char *path)
{
   ImageBuffer buf;
   GfxTexture zero = { 0, 0, 0, 0 };
   if (decodeImageToBuffer(path, &buf) != 0) return zero;
   GfxTexture t = uploadImageBuffer(&buf);
   freeImageBuffer(&buf);
   return t;
}

// Synchronous load of an in-memory PNG/JPEG (e.g. an .rpk entry) into a VRAM texture.
// Format is sniffed from the magic bytes -- no temp file, no filesystem touch.
GfxTexture loadGfxTextureMem(const void *data, uint32_t size)
{
   ImageBuffer buf;
   GfxTexture zero = { 0, 0, 0, 0 };
   if (decodeImageMem(data, size, &buf) != 0) return zero;
   GfxTexture t = uploadImageBuffer(&buf);
   freeImageBuffer(&buf);
   return t;
}

// ---------------------------------------------------------------------------
// background worker
// ---------------------------------------------------------------------------
// A single persistent worker decodes one request at a time. The main thread
// publishes a request (path + generation); the worker decodes and publishes the
// result under that generation. A new request bumps the generation, so a result
// for a superseded request is dropped. A blocking codec call can't be aborted
// mid-decode, so a cancel simply discards the finished result and the worker
// moves on to the newest request. All shared fields are guarded by imgLock.

static sys_lwmutex_t imgLock;
static int           imgInit;          // lock created + worker started
static volatile int  workerStarted;

static char     reqPath[MAX_PATH_LEN];
static uint32_t reqGen;                // newest request id
static int      reqPending;            // a new request awaits the worker

static ImageBuffer resBuf;             // decoded pixels for resGen
static uint32_t    resGen;             // generation resBuf/resState belong to
static int         resState;           // 0 none, 1 ok, -1 failed

static void imgWorker(uint64_t arg)
{
   (void)arg;
   for (;;) {
      char     path[MAX_PATH_LEN];
      uint32_t gen = 0;
      int      have = 0;

      lock(&imgLock);
      if (reqPending) {
         strCopy(path, sizeof path, reqPath);
         gen = reqGen;
         reqPending = 0;
         have = 1;
      }
      unlock(&imgLock);

      if (!have) { sleepMs(8); continue; }

      ImageBuffer b = { 0, 0, 0, 0 };
      int ok = (decodeImageToBuffer(path, &b) == 0);

      lock(&imgLock);
      if (gen == reqGen) {
         // still the request the main thread is waiting on
         if (resState == 1) freeImageBuffer(&resBuf);  // drop an unconsumed prior result
         resBuf   = b;
         resGen   = gen;
         resState = ok ? 1 : -1;
      } else {
         // superseded while decoding: throw it away
         if (ok) freeImageBuffer(&b);
      }
      unlock(&imgLock);
   }
}

static void ensureWorker(void)
{
   if (!imgInit) {
      createLock(&imgLock);
      imgInit = 1;
   }
   if (!workerStarted) {
      sys_ppu_thread_t tid;
      if (spawnThread(&tid, imgWorker, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "img-decode") == 0)
         workerStarted = 1;
   }
}

void requestImageAsync(const char *path)
{
   ensureWorker();

   lock(&imgLock);
   strCopy(reqPath, sizeof reqPath, path);
   reqGen++;
   reqPending = 1;
   // any result still sitting around belongs to an older request now
   if (resState == 1) freeImageBuffer(&resBuf);
   resState = 0;
   unlock(&imgLock);
}

int pollImageAsync(ImageBuffer *out)
{
   int r = 0;
   if (!imgInit) return 0;  // nothing has been requested yet
   lock(&imgLock);
   if (resState != 0 && resGen == reqGen) {
      if (resState == 1) {
         *out = resBuf;
         resBuf.pixels = NULL;
         r = 1;
      } else {
         r = -1;
      }
      resState = 0;
   }
   unlock(&imgLock);
   return r;
}
