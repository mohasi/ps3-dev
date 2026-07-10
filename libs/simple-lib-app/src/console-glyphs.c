// console-glyphs - runtime decode of the PS3 system button glyphs (see console-glyphs.h).
//
// imagefont.bin format (big-endian, which is PS3-native so the structs are read directly):
//   header  { u16 bitOrder; u16 entryCount; u32 indexStart; }
//   index[] { u32 paletteStart; u16 paletteCompSize; u16 paletteDecompSize; u16 codepoint; u16 w; u16 h; u16 _; }
// each index entry points at a zlib-compressed palette block: PaletteHeader + FrameInfo + u32 colors[], plus
// a separate zlib-compressed run of one palette index per pixel. colors are 0xRRGGBBAA.

#include "ui/console-glyphs.h"

#include "vfs.h"    // statPath / openFs / readFs / closeFs
#include "zip.h"    // tinfl_decompress (vendored, heap-free)
#include "dbg.h"    // logInfo / logError
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define IMAGEFONT_PATH "/dev_flash/vsh/resource/imagefont.bin"

typedef struct { uint16_t bitOrder; uint16_t entryCount; uint32_t indexStart; } ImageFontHeader;
typedef struct { uint32_t paletteStart; uint16_t paletteCompSize; uint16_t paletteDecompSize;
                 uint16_t codepoint; uint16_t width; uint16_t height; uint16_t reserved; } ImageFontIndex;
typedef struct { uint16_t colorCount; uint8_t colorChannel; uint8_t frameCount; uint16_t animTime; } PaletteHeader;
typedef struct { uint32_t frameOffset; uint16_t frameLength; uint16_t frameTime;
                 uint8_t reserved1; uint8_t alphaMask; uint16_t reserved2; } FrameInfo;

static const uint16_t GLYPH_CODEPOINTS[GLYPH_COUNT] = {
   [GLYPH_CROSS] = 0xF881, [GLYPH_CIRCLE] = 0xF880, [GLYPH_SQUARE] = 0xF882, [GLYPH_TRIANGLE] = 0xF883,
   [GLYPH_L1]    = 0xF888, [GLYPH_R1]     = 0xF88B, [GLYPH_L2]     = 0xF889, [GLYPH_R2]       = 0xF88C,
   [GLYPH_SELECT] = 0xF88E, [GLYPH_START] = 0xF88F, [GLYPH_R3]     = 0xF88D,
};

static GfxTexture glyphTextures[GLYPH_COUNT];
static int        glyphsLoaded;

// one-shot zlib inflate via the vendored tinfl (heap-free), so nothing here reaches into libc-heavy code.
// dst must be sized for the full output; decode runs once at startup on one thread, so a static state is fine.
static int inflateZlib(const void *src, uint32_t srcLen, void *dst, uint32_t dstCap)
{
   static tinfl_decompressor decomp;
   tinfl_init(&decomp);
   size_t in = srcLen, out = dstCap;
   tinfl_status status = tinfl_decompress(&decomp, (const mz_uint8 *)src, &in, (mz_uint8 *)dst, (mz_uint8 *)dst, &out,
                                          TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
   return status == TINFL_STATUS_DONE ? 0 : -1;
}

// read the whole system font into a heap buffer (caller frees). returns bytes read, 0 on failure.
static uint32_t readImageFont(uint8_t **outFont)
{
   VfsStat stat;
   if (statPath(IMAGEFONT_PATH, &stat) != 0 || stat.size == 0) return 0;

   uint8_t *font = (uint8_t *)malloc(stat.size);
   if (!font) return 0;

   VfsFile file;
   if (openFs(IMAGEFONT_PATH, VFS_O_RDONLY, &file) != 0) { free(font); return 0; }
   int64_t read = readFs(&file, font, stat.size);
   closeFs(&file);

   if (read != (int64_t)stat.size) { free(font); return 0; }
   *outFont = font;
   return (uint32_t)stat.size;
}

static const ImageFontIndex *findGlyph(const uint8_t *font, uint16_t codepoint)
{
   const ImageFontHeader *header = (const ImageFontHeader *)font;
   const ImageFontIndex *index = (const ImageFontIndex *)(font + header->indexStart);
   for (int i = 0; i < header->entryCount; i++)
      if (index[i].codepoint == codepoint) return &index[i];
   return NULL;
}

// decode one glyph (palette + indexed pixels) into an A8R8G8B8 texture. 0 ok.
static int decodeGlyph(const uint8_t *font, uint16_t codepoint, GfxTexture *out)
{
   const ImageFontIndex *entry = findGlyph(font, codepoint);
   if (!entry) return -1;

   // inflate the palette block (header + frame info + colors)
   uint8_t *palette = (uint8_t *)malloc(entry->paletteDecompSize);
   if (!palette) return -1;
   if (inflateZlib(font + entry->paletteStart, entry->paletteCompSize, palette, entry->paletteDecompSize) != 0) {
      free(palette);
      return -1;
   }
   const FrameInfo *frame  = (const FrameInfo *)(palette + sizeof(PaletteHeader));
   const uint32_t  *colors = (const uint32_t *)(palette + sizeof(PaletteHeader) + sizeof(FrameInfo));

   // inflate the one-byte-per-pixel indices, then map through the palette to A8R8G8B8
   int pixels = entry->width * entry->height;
   uint8_t  *indices = (uint8_t *)malloc(pixels);
   uint32_t *argb    = (uint32_t *)malloc((size_t)pixels * 4);
   int ok = indices && argb &&
            inflateZlib(font + frame->frameOffset, frame->frameLength, indices, (uint32_t)pixels) == 0;
   if (ok) {
      for (int i = 0; i < pixels; i++) {
         uint32_t color = colors[indices[i]];              // 0xRRGGBBAA as stored
         argb[i] = (color >> 8) | (color << 24);           // -> 0xAARRGGBB (native A8R8G8B8 byte order)
      }
      out->w     = entry->width;
      out->h     = entry->height;
      out->pitch = (int)(((uint32_t)(entry->width * 4) + 63) & ~63u);
      out->offset = uploadGfxTexture(argb, entry->width, entry->height, entry->width * 4);
   }
   free(indices);
   free(argb);
   free(palette);
   return (ok && out->offset) ? 0 : -1;
}

int loadConsoleGlyphs(void)
{
   if (glyphsLoaded) return 0;

   uint8_t *font = NULL;
   if (readImageFont(&font) == 0) { logError("[glyphs] cannot read %s\n", IMAGEFONT_PATH); return -1; }

   int decoded = 0;
   for (int glyph = 0; glyph < GLYPH_COUNT; glyph++)
      if (decodeGlyph(font, GLYPH_CODEPOINTS[glyph], &glyphTextures[glyph]) == 0) decoded++;
   free(font);

   glyphsLoaded = 1;
   logInfo("[glyphs] decoded %d/%d button glyphs\n", decoded, GLYPH_COUNT);
   return decoded > 0 ? 0 : -1;
}

GfxTexture getConsoleGlyph(ConsoleGlyph glyph)
{
   if (glyph < 0 || glyph >= GLYPH_COUNT) { GfxTexture empty = {0}; return empty; }
   return glyphTextures[glyph];
}

void freeConsoleGlyphs(void)
{
   if (!glyphsLoaded) return;
   finishGfx();
   for (int glyph = 0; glyph < GLYPH_COUNT; glyph++) freeGfxTexture(&glyphTextures[glyph]);
   memset(glyphTextures, 0, sizeof glyphTextures);
   glyphsLoaded = 0;
}
