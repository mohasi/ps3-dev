// image-encoder - PNG encode via the SDK codec (cellPngEnc), the symmetric counterpart to
// image-loader.c's cellPngDec decode. Kept in its own translation unit so that only apps which
// actually save PNGs pull in the libpngenc_stub dependency (the image viewer, which only decodes,
// does not). Flow mirrors the SDK sample sdk/samples/codec/png_enc/png_enc.c.
#include "image-loader.h"   // savePngArgb declaration

#include <stdlib.h>
#include <string.h>
#include <cell/codec/pngenc.h>
#include <cell/sysmodule.h>   // CELL_SYSMODULE_PNGENC (the encoder PRX, like PNGDEC for decode)

#include "dbg.h"   // logError / logInfo

// PPU-only encode (no SPU/SPURS): simplest, and a thumbnail is tiny. Priorities match the decoder.
#define ENC_PPU_PRIO 512
#define ENC_SPU_PRIO 200

static int pngEncModuleLoaded = 0;   // CELL_SYSMODULE_PNGENC loaded? (loaded lazily on first encode)

// Encodes a w*h ARGB8888 image (A8R8G8B8 byte order: A,R,G,B -- the lib's native surface layout) to
// a PNG file at `path`. Alpha is forced opaque so screenshots are always viewable. Returns 0 on
// success. Synchronous (blocks on the codec); call off the render path (e.g. at save time).
int savePngArgb(const char *path, const void *argb, int w, int h)
{
    if (!path || !argb || w <= 0 || h <= 0) return -1;

    // The PNGENC PRX must be loaded before any cellPngEnc call (the decoder loads PNGDEC the same
    // way in gfx.c); without it cellPngEncOpen returns CELL_PNGENC_ERROR_ARG. Lazy + once.
    if (!pngEncModuleLoaded)
    {
        int m = cellSysmoduleLoadModule(CELL_SYSMODULE_PNGENC);
        if (m != CELL_OK && m != (int)CELL_SYSMODULE_ERROR_DUPLICATED) { logError("[png-enc] LoadModule=0x%08x\n", m); return -1; }
        pngEncModuleLoaded = 1;
    }

    // Work on an opaque copy: a screenshot must not come out partly transparent if the scene's
    // render left non-255 alpha in the framebuffer. (A,R,G,B -> byte 0 of each pixel is alpha.)
    size_t bytes = (size_t)w * (size_t)h * 4u;
    unsigned char *opaque = (unsigned char *)malloc(bytes);
    if (!opaque) return -1;
    memcpy(opaque, argb, bytes);
    for (size_t i = 0; i < bytes; i += 4) opaque[i] = 0xff;

    CellPngEncConfig   config;
    CellPngEncAttr     attr;
    CellPngEncResource resource;
    CellPngEncHandle   handle;
    CellPngEncPicture  picture;
    CellPngEncEncodeParam encodeParam;
    CellPngEncOutputParam outputParam;
    CellPngEncStreamInfo  streamInfo;
    uint32_t streamInfoNum = 0;
    int ret, rc = -1;

    memset(&config, 0, sizeof config);
    config.maxWidth    = (uint32_t)w;
    config.maxHeight   = (uint32_t)h;
    config.maxBitDepth = 8;
    config.enableSpu   = false;
    config.addMemSize  = 0;
    config.exParamList = NULL;
    config.exParamNum  = 0;

    ret = cellPngEncQueryAttr(&config, &attr);
    if (ret < CELL_OK) { logError("[png-enc] QueryAttr=0x%08x (%dx%d)\n", ret, w, h); free(opaque); return -1; }

    resource.memAddr           = malloc(attr.memSize);
    resource.memSize           = attr.memSize;
    resource.ppuThreadPriority = ENC_PPU_PRIO;
    resource.spuThreadPriority = ENC_SPU_PRIO;
    if (!resource.memAddr) { logError("[png-enc] malloc %u failed\n", (unsigned)attr.memSize); free(opaque); return -1; }

    ret = cellPngEncOpen(&config, &resource, &handle);
    if (ret < CELL_OK) { logError("[png-enc] Open=0x%08x\n", ret); free(resource.memAddr); free(opaque); return -1; }

    ret = cellPngEncWaitForInput(handle, true);
    if (ret < CELL_OK) { logError("[png-enc] WaitForInput=0x%08x\n", ret); goto close; }

    memset(&picture, 0, sizeof picture);
    picture.width       = (uint32_t)w;
    picture.height      = (uint32_t)h;
    picture.pitchWidth  = (uint32_t)(w * 4);            // bytes per row (= width * components)
    picture.colorSpace  = CELL_PNGENC_COLOR_SPACE_ARGB; // our buffer is A8R8G8B8
    picture.bitDepth    = 8;
    picture.packedPixel = false;
    picture.pictureAddr = opaque;
    picture.userData    = 0;

    memset(&encodeParam, 0, sizeof encodeParam);
    encodeParam.enableSpu         = false;
    encodeParam.encodeColorSpace  = CELL_PNGENC_COLOR_SPACE_ARGB;   // matches the input (sample convention)
    encodeParam.compressionLevel  = CELL_PNGENC_COMPR_LEVEL_6;
    encodeParam.filterType        = CELL_PNGENC_FILTER_TYPE_NONE;
    encodeParam.ancillaryChunkList = NULL;
    encodeParam.ancillaryChunkNum  = 0;

    memset(&outputParam, 0, sizeof outputParam);
    outputParam.location       = CELL_PNGENC_LOCATION_FILE;
    outputParam.streamFileName = path;
    outputParam.streamAddr     = NULL;
    outputParam.limitSize      = 0;

    ret = cellPngEncEncodePicture(handle, &picture, &encodeParam, &outputParam);
    if (ret < CELL_OK) { logError("[png-enc] EncodePicture=0x%08x\n", ret); goto close; }
    ret = cellPngEncWaitForOutput(handle, &streamInfoNum, true);
    if (ret < CELL_OK) { logError("[png-enc] WaitForOutput=0x%08x\n", ret); goto close; }
    if (cellPngEncGetStreamInfo(handle, &streamInfo) < CELL_OK) { logError("[png-enc] GetStreamInfo failed\n"); goto close; }
    if (streamInfo.state < CELL_OK) { logError("[png-enc] state=0x%08x\n", streamInfo.state); goto close; }
    logInfo("[png-enc] wrote %s (%dx%d, %u bytes)\n", path, w, h, (unsigned)streamInfo.streamSize);
    rc = 0;

close:
    cellPngEncClose(handle);
    free(resource.memAddr);
    free(opaque);
    return rc;
}
