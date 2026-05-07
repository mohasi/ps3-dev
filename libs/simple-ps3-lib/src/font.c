// font - truetype font rendering via libfont/freetype
#include "font.h"
#include <cell/font.h>
#include <cell/fontFT.h>
#include <cell/sysmodule.h>
#include <cell/cell_fs.h>
#include <string.h>
#include <stdlib.h>
#include "gfx.h"
#include "colors.h"

static const CellFontLibrary *fontLib = NULL;
static CellFontRenderer       fontRenderer;
static int                    fontInited = 0;

static void *fontMalloc(void *obj, uint32_t size) { (void)obj; return malloc(size); }
static void  fontFree(void *obj, void *p) { (void)obj; free(p); }
static void *fontRealloc(void *obj, void *p, uint32_t size) { (void)obj; return realloc(p, size); }
static void *fontCalloc(void *obj, uint32_t n, uint32_t sz) { (void)obj; return calloc(n, sz); }

int fontInit(void)
{
    if (fontInited) return 0;

    cellSysmoduleLoadModule(CELL_SYSMODULE_FONT);
    cellSysmoduleLoadModule(CELL_SYSMODULE_FREETYPE);
    cellSysmoduleLoadModule(CELL_SYSMODULE_FONTFT);

    static uint32_t fontFileCache[256 * 1024 / sizeof(uint32_t)];
    CellFontConfig fconfig;
    CellFontConfig_initialize(&fconfig);
    fconfig.FileCache.buffer = fontFileCache;
    fconfig.FileCache.size   = sizeof(fontFileCache);
    fconfig.flags = 0;
    if (cellFontInit(&fconfig) != CELL_OK) return -1;

    CellFontLibraryConfigFT config;
    CellFontLibraryConfigFT_initialize(&config);
    config.MemoryIF.Object  = NULL;
    config.MemoryIF.Malloc  = fontMalloc;
    config.MemoryIF.Free    = fontFree;
    config.MemoryIF.Realloc = fontRealloc;
    config.MemoryIF.Calloc  = fontCalloc;

    if (cellFontInitLibraryFreeType(&config, &fontLib) != CELL_OK) return -1;

    CellFontRendererConfig rconfig;
    CellFontRendererConfig_initialize(&rconfig);
    CellFontRendererConfig_setAllocateBuffer(&rconfig, 1024 * 64, 0);
    if (cellFontCreateRenderer(fontLib, &rconfig, &fontRenderer) != CELL_OK) return -1;

    fontInited = 1;
    return 0;
}

void fontTerm(void)
{
    if (!fontInited) return;
    cellFontDestroyRenderer(&fontRenderer);
    cellFontEndLibrary(fontLib);
    cellFontEnd();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_FONTFT);
    cellSysmoduleUnloadModule(CELL_SYSMODULE_FREETYPE);
    cellSysmoduleUnloadModule(CELL_SYSMODULE_FONT);
    fontInited = 0;
}

Font fontOpenSystem(int type)
{
    Font f;
    memset(&f, 0, sizeof(f));

    if (!fontInited) fontInit();

    int fontType;
    switch (type) {
        case FONT_POP:       fontType = CELL_FONT_TYPE_DEFAULT_GOTHIC_LATIN_SET; break;
        case FONT_GOTHIC_JP: fontType = CELL_FONT_TYPE_DEFAULT_GOTHIC_JP_SET; break;
        case FONT_SANS:      fontType = CELL_FONT_TYPE_DEFAULT_SANS_SERIF; break;
        case FONT_SERIF:     fontType = CELL_FONT_TYPE_DEFAULT_SERIF; break;
        default:             fontType = CELL_FONT_TYPE_DEFAULT_GOTHIC_LATIN_SET; break;
    }

    CellFontType ft;
    ft.type = fontType;
    ft.map  = CELL_FONT_MAP_UNICODE;

    if (cellFontOpenFontset(fontLib, &ft, &f.font) == CELL_OK) {
        f.open = 1;
        cellFontSetResolutionDpi(&f.font, 72, 72);
    }

    return f;
}

Font fontOpenFile(const char *path)
{
    Font f;
    memset(&f, 0, sizeof(f));

    if (!fontInited) fontInit();

    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return f;

    CellFsStat stat;
    if (cellFsFstat(fd, &stat) != CELL_FS_SUCCEEDED) { cellFsClose(fd); return f; }

    uint32_t size = (uint32_t)stat.st_size;
    void *buf = malloc(size);
    if (!buf) { cellFsClose(fd); return f; }

    uint64_t nread;
    cellFsRead(fd, buf, size, &nread);
    cellFsClose(fd);

    if (cellFontOpenFontMemory(fontLib, buf, size, 0, 0, &f.font) == CELL_OK) {
        f.open = 1;
        cellFontSetResolutionDpi(&f.font, 72, 72);
    } else {
        free(buf);
    }

    return f;
}

void fontClose(Font *f)
{
    if (f->open) {
        cellFontCloseFont(&f->font);
        f->open = 0;
    }
}

float fontMeasureText(Font *f, int size, const char *text)
{
    if (!f->open || !text) return 0.0f;
    float fsize = (float)size;
    cellFontSetScalePixel(&f->font, fsize, fsize);

    float w = 0.0f;
    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        CellFontGlyphMetrics metrics;
        if (cellFontGetCharGlyphMetrics(&f->font, *p, &metrics) == CELL_OK)
            w += metrics.Horizontal.advance;
        else
            w += fsize;
        p++;
    }
    return w;
}

float fontMeasureChar(Font *f, int size, uint32_t code)
{
    float fsize = (float)size;
    cellFontSetScalePixel(&f->font, fsize, fsize);
    CellFontGlyphMetrics metrics;
    if (cellFontGetCharGlyphMetrics(&f->font, code, &metrics) == CELL_OK)
        return metrics.Horizontal.advance;
    return fsize;
}

void fontDraw(int x, int y, int width, int height, const char *text, Font *f, int size, uint32_t color, TextWrap wrap)
{
    if (!f->open || !text || !text[0]) return;

    float fsize = (float)size;
    cellFontSetScalePixel(&f->font, fsize, fsize);
    cellFontBindRenderer(&f->font, &fontRenderer);

    CellFontHorizontalLayout layout;
    cellFontGetHorizontalLayout(&f->font, &layout);
    float lineH = layout.lineHeight;
    float baseY = layout.baseLineY;

    int lineCount = 1;
    if (wrap == TEXT_WRAP && width > 0) {
        float px = 0.0f;
        const uint8_t *sp = (const uint8_t *)text;
        while (*sp) {
            if (*sp == '\n') {
                lineCount++;
                px = 0.0f;
                sp++;
                continue;
            }
            CellFontGlyphMetrics sm;
            float adv = fsize;
            if (cellFontGetCharGlyphMetrics(&f->font, *sp, &sm) == CELL_OK)
                adv = sm.Horizontal.advance;
            if (*sp == ' ') {
                float wordW = 0.0f;
                const uint8_t *wp = sp + 1;
                while (*wp && *wp != ' ' && *wp != '\n') {
                    CellFontGlyphMetrics wm;
                    if (cellFontGetCharGlyphMetrics(&f->font, *wp, &wm) == CELL_OK)
                        wordW += wm.Horizontal.advance;
                    else
                        wordW += fsize;
                    wp++;
                }
                if (px + adv + wordW > (float)width && px > 0.0f) {
                    lineCount++;
                    px = 0.0f;
                    sp++;
                    continue;
                }
            }
            px += adv;
            sp++;
        }
    }

    int surfW = (width > 0) ? width : FONT_MAX_RENDER_W;
    int surfH = (int)(lineH * lineCount + fsize);
    if (height > 0 && surfH > height) surfH = height;

    int bufSize = surfW * surfH * 4;
    uint8_t *buf = (uint8_t *)malloc(bufSize);
    if (!buf) return;
    memset(buf, 0, bufSize);

    CellFontRenderSurface surf;
    cellFontRenderSurfaceInit(&surf, buf, surfW * 4, 4, surfW, surfH);
    cellFontRenderSurfaceSetScissor(&surf, 0, 0, surfW, surfH);
    cellFontSetupRenderScalePixel(&f->font, fsize, fsize);

    float penX = 0.0f;
    float penY = baseY;
    int maxX = 0;
    const uint8_t *p = (const uint8_t *)text;

    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >> 8) & 0xFF;
    uint8_t cb = color & 0xFF;

    float ellipsisW = 0.0f;
    if (wrap == TEXT_NOWRAP_ELLIPSIS && width > 0) {
        for (int i = 0; i < 3; i++)
            ellipsisW += fontMeasureChar(f, size, '.');
        cellFontSetScalePixel(&f->font, fsize, fsize);
        cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
    }

    while (*p) {
        uint32_t code = *p;

        if (code == '\n') {
            if (wrap == TEXT_WRAP) {
                penX = 0.0f;
                penY += lineH;
                if (height > 0 && penY + lineH > (float)surfH) break;
            }
            p++;
            continue;
        }

        CellFontGlyphMetrics metrics;
        float advance = fsize;
        if (cellFontGetCharGlyphMetrics(&f->font, code, &metrics) == CELL_OK)
            advance = metrics.Horizontal.advance;

        if (width > 0) {
            if (wrap == TEXT_WRAP) {
                if (code == ' ') {
                    float wordW = 0.0f;
                    const uint8_t *wp = p + 1;
                    while (*wp && *wp != ' ' && *wp != '\n') {
                        CellFontGlyphMetrics wm;
                        if (cellFontGetCharGlyphMetrics(&f->font, *wp, &wm) == CELL_OK)
                            wordW += wm.Horizontal.advance;
                        else
                            wordW += fsize;
                        wp++;
                    }
                    if (penX + advance + wordW > (float)width && penX > 0.0f) {
                        penX = 0.0f;
                        penY += lineH;
                        if (height > 0 && penY + lineH > (float)surfH) break;
                        p++;
                        continue;
                    }
                }
            } else if (wrap == TEXT_NOWRAP_ELLIPSIS) {
                if (penX + advance > (float)width - ellipsisW && *(p + 1)) {
                    for (int i = 0; i < 3; i++) {
                        CellFontImageTransInfo ti;
                        CellFontGlyphMetrics dm;
                        if (cellFontRenderCharGlyphImage(&f->font, '.', &surf, penX, penY, &dm, &ti) == CELL_OK) {
                            uint8_t *img = ti.Image;
                            for (int iy = 0; iy < ti.imageHeight; iy++) {
                                uint8_t *dst = ((uint8_t *)ti.Surface) + ti.surfWidthByte * iy;
                                for (int ix = 0; ix < ti.imageWidth; ix++) {
                                    uint8_t a = img[iy * ti.imageWidthByte + ix];
                                    if (a) { dst[ix*4]=a; dst[ix*4+1]=cr; dst[ix*4+2]=cg; dst[ix*4+3]=cb; }
                                }
                            }
                            penX += dm.Horizontal.advance;
                        }
                    }
                    int endX = (int)(penX + 0.5f);
                    if (endX > maxX) maxX = endX;
                    break;
                }
            } else {
                if (penX + advance > (float)width) break;
            }
        }

        CellFontImageTransInfo transInfo;
        int ret = cellFontRenderCharGlyphImage(&f->font, code, &surf, penX, penY, &metrics, &transInfo);
        if (ret == CELL_OK) {
            uint8_t *img = transInfo.Image;
            int imgW = transInfo.imageWidth;
            int imgH = transInfo.imageHeight;
            int imgBW = transInfo.imageWidthByte;
            int surfBW = transInfo.surfWidthByte;

            for (int iy = 0; iy < imgH; iy++) {
                uint8_t *dst = ((uint8_t *)transInfo.Surface) + surfBW * iy;
                for (int ix = 0; ix < imgW; ix++) {
                    uint8_t a = img[iy * imgBW + ix];
                    if (a) {
                        dst[ix * 4 + 0] = a;
                        dst[ix * 4 + 1] = cr;
                        dst[ix * 4 + 2] = cg;
                        dst[ix * 4 + 3] = cb;
                    }
                }
            }

            penX += metrics.Horizontal.advance;
            int endX = (int)(penX + 0.5f);
            if (endX > maxX) maxX = endX;
        } else {
            penX += fsize;
        }

        p++;
    }

    int drawW = maxX;
    int drawH = surfH;
    if (drawW > 0 && drawH > 0) {
        GfxTexture tex;
        tex.w = drawW;
        tex.h = drawH;
        tex.offset = gfxUploadTexture(buf, drawW, drawH, surfW * 4, VRAM_TEMP);

        if (tex.offset != 0) {
            gfxDrawSprite(x, y, drawW, drawH, tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
        }
    }

    free(buf);
}

GfxTexture fontToTexture(int width, int height, const char *text, Font *f, int size, uint32_t color, TextWrap wrap)
{
    GfxTexture result = { 0, 0, 0 };

    if (!f->open || !text || !text[0]) return result;

    float fsize = (float)size;
    cellFontSetScalePixel(&f->font, fsize, fsize);
    cellFontBindRenderer(&f->font, &fontRenderer);

    CellFontHorizontalLayout layout;
    cellFontGetHorizontalLayout(&f->font, &layout);
    float lineH = layout.lineHeight;
    float baseY = layout.baseLineY;

    int lineCount = 1;
    if (wrap == TEXT_WRAP && width > 0) {
        float px = 0.0f;
        const uint8_t *sp = (const uint8_t *)text;
        while (*sp) {
            if (*sp == '\n') { lineCount++; px = 0.0f; sp++; continue; }
            CellFontGlyphMetrics sm;
            float adv = fsize;
            if (cellFontGetCharGlyphMetrics(&f->font, *sp, &sm) == CELL_OK) adv = sm.Horizontal.advance;
            if (*sp == ' ') {
                float wordW = 0.0f;
                const uint8_t *wp = sp + 1;
                while (*wp && *wp != ' ' && *wp != '\n') {
                    CellFontGlyphMetrics wm;
                    if (cellFontGetCharGlyphMetrics(&f->font, *wp, &wm) == CELL_OK) wordW += wm.Horizontal.advance;
                    else wordW += fsize;
                    wp++;
                }
                if (px + adv + wordW > (float)width && px > 0.0f) { lineCount++; px = 0.0f; sp++; continue; }
            }
            px += adv;
            sp++;
        }
    }

    int surfW = (width > 0) ? width : FONT_MAX_RENDER_W;
    int surfH = (int)(lineH * lineCount + fsize);
    if (height > 0 && surfH > height) surfH = height;

    int bufSize = surfW * surfH * 4;
    uint8_t *buf = (uint8_t *)malloc(bufSize);
    if (!buf) return result;
    memset(buf, 0, bufSize);

    CellFontRenderSurface surf;
    cellFontRenderSurfaceInit(&surf, buf, surfW * 4, 4, surfW, surfH);
    cellFontRenderSurfaceSetScissor(&surf, 0, 0, surfW, surfH);
    cellFontSetupRenderScalePixel(&f->font, fsize, fsize);

    float penX = 0.0f;
    float penY = baseY;
    int maxX = 0;
    const uint8_t *p = (const uint8_t *)text;

    uint8_t cr = (color >> 16) & 0xFF;
    uint8_t cg = (color >> 8) & 0xFF;
    uint8_t cb = color & 0xFF;

    float ellipsisW = 0.0f;
    if (wrap == TEXT_NOWRAP_ELLIPSIS && width > 0) {
        for (int i = 0; i < 3; i++) ellipsisW += fontMeasureChar(f, size, '.');
        cellFontSetScalePixel(&f->font, fsize, fsize);
        cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
    }

    while (*p) {
        uint32_t code = *p;

        if (code == '\n') {
            if (wrap == TEXT_WRAP) { penX = 0.0f; penY += lineH; if (height > 0 && penY + lineH > (float)surfH) break; }
            p++; continue;
        }

        CellFontGlyphMetrics metrics;
        float advance = fsize;
        if (cellFontGetCharGlyphMetrics(&f->font, code, &metrics) == CELL_OK) advance = metrics.Horizontal.advance;

        if (width > 0) {
            if (wrap == TEXT_WRAP) {
                if (code == ' ') {
                    float wordW = 0.0f;
                    const uint8_t *wp = p + 1;
                    while (*wp && *wp != ' ' && *wp != '\n') {
                        CellFontGlyphMetrics wm;
                        if (cellFontGetCharGlyphMetrics(&f->font, *wp, &wm) == CELL_OK) wordW += wm.Horizontal.advance;
                        else wordW += fsize;
                        wp++;
                    }
                    if (penX + advance + wordW > (float)width && penX > 0.0f) { penX = 0.0f; penY += lineH; if (height > 0 && penY + lineH > (float)surfH) break; p++; continue; }
                }
            } else if (wrap == TEXT_NOWRAP_ELLIPSIS) {
                if (penX + advance > (float)width - ellipsisW && *(p + 1)) {
                    for (int i = 0; i < 3; i++) {
                        CellFontImageTransInfo ti;
                        CellFontGlyphMetrics dm;
                        if (cellFontRenderCharGlyphImage(&f->font, '.', &surf, penX, penY, &dm, &ti) == CELL_OK) {
                            uint8_t *img = ti.Image;
                            for (int iy = 0; iy < ti.imageHeight; iy++) {
                                uint8_t *dst = ((uint8_t *)ti.Surface) + ti.surfWidthByte * iy;
                                for (int ix = 0; ix < ti.imageWidth; ix++) {
                                    uint8_t a = img[iy * ti.imageWidthByte + ix];
                                    if (a) { dst[ix*4]=a; dst[ix*4+1]=cr; dst[ix*4+2]=cg; dst[ix*4+3]=cb; }
                                }
                            }
                            penX += dm.Horizontal.advance;
                        }
                    }
                    int endX = (int)(penX + 0.5f); if (endX > maxX) maxX = endX;
                    break;
                }
            } else {
                if (penX + advance > (float)width) break;
            }
        }

        CellFontImageTransInfo transInfo;
        int ret = cellFontRenderCharGlyphImage(&f->font, code, &surf, penX, penY, &metrics, &transInfo);
        if (ret == CELL_OK) {
            uint8_t *img = transInfo.Image;
            int imgW = transInfo.imageWidth;
            int imgH = transInfo.imageHeight;
            int imgBW = transInfo.imageWidthByte;
            int surfBW = transInfo.surfWidthByte;
            for (int iy = 0; iy < imgH; iy++) {
                uint8_t *dst = ((uint8_t *)transInfo.Surface) + surfBW * iy;
                for (int ix = 0; ix < imgW; ix++) {
                    uint8_t a = img[iy * imgBW + ix];
                    if (a) { dst[ix*4]=a; dst[ix*4+1]=cr; dst[ix*4+2]=cg; dst[ix*4+3]=cb; }
                }
            }
            penX += metrics.Horizontal.advance;
            int endX = (int)(penX + 0.5f); if (endX > maxX) maxX = endX;
        } else {
            penX += fsize;
        }
        p++;
    }

    int drawW = maxX;
    int drawH = surfH;
    if (drawW > 0 && drawH > 0) {
        result.w = drawW;
        result.h = drawH;
        result.offset = gfxUploadTexture(buf, drawW, drawH, surfW * 4, VRAM_PERM);
    }

    free(buf);
    return result;
}
