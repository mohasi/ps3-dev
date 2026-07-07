// font - truetype font rendering via libfont/freetype
#include "font.h"
#include <cell/font.h>
#include <cell/fontFT.h>
#include <cell/sysmodule.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "gfx.h"
#include "colors.h"
#include "dbg.h"
#include "vfs.h"

static const int FONT_MAX_RENDER_W = 1024;
static const size_t FONT_MAX_SURFACE_BYTES = 16u * 1024u * 1024u;   // sanity cap for the rasterize() scratch buffer

static const CellFontLibrary *fontLib = NULL;
static CellFontRenderer       fontRenderer;
static int                    fontInited = 0;

static void *fontMalloc(void *obj, uint32_t size) { (void)obj; return malloc(size); }
static void  fontFree(void *obj, void *p) { (void)obj; free(p); }
static void *fontRealloc(void *obj, void *p, uint32_t size) { (void)obj; return realloc(p, size); }
static void *fontCalloc(void *obj, uint32_t n, uint32_t sz) { (void)obj; return calloc(n, sz); }

int initFont(void)
{
   if (fontInited) return 0;

   cellSysmoduleLoadModule(CELL_SYSMODULE_FONT);
   cellSysmoduleLoadModule(CELL_SYSMODULE_FREETYPE);
   cellSysmoduleLoadModule(CELL_SYSMODULE_FONTFT);

   static uint32_t fontFileCache[256 * 1024 / sizeof(uint32_t)];
   // Slots for user fonts opened via openFontFile/openFontMemory. CellFontConfig_initialize
   // defaults userFontEntryMax to 0, so without this any user-font open returns
   // CELL_FONT_ERROR_FONT_OPEN_MAX (0x8054000d). (System fonts don't use these slots.)
   static CellFontEntry userFontEntries[8];

   CellFontConfig fconfig;
   CellFontConfig_initialize(&fconfig);
   fconfig.FileCache.buffer = fontFileCache;
   fconfig.FileCache.size   = sizeof(fontFileCache);
   fconfig.flags = 0;
   fconfig.userFontEntryMax  = 8;
   fconfig.userFontEntrys    = userFontEntries;
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

void termFont(void)
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

Font openSystemFont(int type)
{
   Font f;
   memset(&f, 0, sizeof(f));

   if (!fontInited) initFont();

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

// cellFont caches opened faces by the `uniqueId` passed to cellFontOpenFont*; if two different
// fonts share an id (the old hardcoded 0) the second open returns the FIRST cached face. Hand out a
// fresh id per open so every font is distinct (e.g. the dialogue font and the in-game chat font).
static int nextFontUid(void) { static int uid = 1; return uid++; }

// Parse the SFNT (TrueType/OpenType) header for the hhea ascender/descender + head unitsPerEm, which is
// what FreeType (and hence Ren'Py, ftfont.pyx) uses for the line height (ascender - descender). cellFont's
// CellFontHorizontalLayout instead uses the OS/2 usWin* metrics, which over-space some OTF fonts. SFNT is
// big-endian. Fills f->unitsPerEm/hheaAscent/hheaDescent; unitsPerEm stays 0 if the tables aren't found.
static uint16_t sfntU16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  sfntS16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }
static uint32_t sfntU32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void parseSfntMetrics(Font *f, const uint8_t *buf, uint32_t size)
{
   f->unitsPerEm = 0; f->hheaAscent = 0; f->hheaDescent = 0;
   if (!buf || size < 12) return;
   uint16_t numTables = sfntU16(buf + 4);     // offset table: u32 tag, u16 numTables, ...
   uint32_t headOff = 0, hheaOff = 0;
   for (uint16_t i = 0; i < numTables; i++)
   {
      uint32_t rec = 12u + (uint32_t)i * 16u;   // each table record: tag(4) checksum(4) offset(4) length(4)
      if (rec + 16u > size) break;
      const uint8_t *tag = buf + rec;
      uint32_t off = sfntU32(buf + rec + 8);
      if (off > size) continue;   // reject bogus offsets before they can overflow the checks below
      if (tag[0]=='h'&&tag[1]=='e'&&tag[2]=='a'&&tag[3]=='d') headOff = off;
      else if (tag[0]=='h'&&tag[1]=='h'&&tag[2]=='e'&&tag[3]=='a') hheaOff = off;
   }
   int upm = (headOff && headOff + 20u <= size) ? sfntU16(buf + headOff + 18) : 0;   // head.unitsPerEm @ +18
   if (hheaOff && hheaOff + 10u <= size && upm > 0)
   {
      f->hheaAscent  = sfntS16(buf + hheaOff + 4);   // hhea.ascender  @ +4
      f->hheaDescent = sfntS16(buf + hheaOff + 6);   // hhea.descender @ +6
      if (f->hheaAscent != 0 || f->hheaDescent != 0) f->unitsPerEm = upm;
   }
}

Font openFontFile(const char *path)
{
   Font f;
   memset(&f, 0, sizeof(f));

   if (!fontInited) initFont();

   // size via path-stat (the VFS has no fstat-by-handle); then open + read.
   VfsStat st;
   if (statPath(path, &st) != 0) return f;
   uint32_t size = (uint32_t)st.size;

   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return f;

   void *buf = malloc(size);
   if (!buf) { closeFs(&file); return f; }

   readFs(&file, buf, size);
   closeFs(&file);

   if (cellFontOpenFontMemory(fontLib, buf, size, 0, nextFontUid(), &f.font) == CELL_OK) {
      f.open = 1;
      f.buffer = buf;  // cellFont keeps reading buf for the font's life; closeFont frees it
      cellFontSetResolutionDpi(&f.font, 72, 72);
      parseSfntMetrics(&f, buf, size);   // hhea/head metrics for the faithful (Ren'Py) line height
   } else {
      free(buf);
   }

   return f;
}

Font openFontMemory(const void *buf, uint32_t size)
{
   Font f;
   memset(&f, 0, sizeof(f));

   if (!fontInited) initFont();
   if (!buf || size == 0) return f;

   // cellFont keeps reading the buffer for the font's lifetime, so own a private copy.
   void *own = malloc(size);
   if (!own) return f;
   memcpy(own, buf, size);

   int ret = cellFontOpenFontMemory(fontLib, own, size, 0, nextFontUid(), &f.font);
   if (ret == CELL_OK) {
      f.open = 1;
      f.buffer = own;  // closeFont frees it
      cellFontSetResolutionDpi(&f.font, 72, 72);
      parseSfntMetrics(&f, (const uint8_t *)own, size);   // hhea/head metrics for the faithful line height
   } else {
      logError("[font] cellFontOpenFontMemory failed: 0x%x (size=%u)\n", ret, size);
      free(own);
   }

   return f;
}

void closeFont(Font *f)
{
   if (f->open) {
      cellFontCloseFont(&f->font);
      f->open = 0;
   }
   free(f->buffer);  // backing memory for openFontFile; NULL (safe) for system fonts
   f->buffer = NULL;
}

void setFontMetrics(Font *f, FontMetricsMode mode, float grid)
{
   f->metricsMode = (int)mode;
   f->metricsGrid = grid > 0.0f ? grid : 0.0f;
}

// Applies the font's advance model to a raw advance (see FontMetricsMode). The small
// epsilon keeps an exact multiple from ceiling up a step due to float error.
static float quantAdvance(const Font *f, float adv)
{
   if (f->metricsMode != FONT_METRICS_GRID_CEIL || f->metricsGrid <= 0.0f) return adv;
   float cells = ceilf(adv / f->metricsGrid - 0.001f);
   return cells * f->metricsGrid;
}

// Kerning between two glyphs at the current scale, per the font's metrics mode.
// NATIVE mode reports 0 (fractional-advance stacks don't kern); GRID_CEIL floors the
// offset to the grid, matching classic stacks' integer `delta.x >> 6`.
static float fontKern(Font *f, uint32_t prev, uint32_t code)
{
   if (f->metricsMode != FONT_METRICS_GRID_CEIL || prev == 0) return 0.0f;
   CellFontKerning k;
   if (cellFontGetKerning(&f->font, prev, code, &k) != CELL_OK) return 0.0f;
   if (k.offsetX == 0.0f) return 0.0f;
   float g = f->metricsGrid > 0.0f ? f->metricsGrid : 1.0f;
   return floorf(k.offsetX / g + 0.001f) * g;
}

// decodes one utf-8 codepoint, advances *pp past it. returns codepoint.
static uint32_t decodeUtf8(const uint8_t **pp)
{
   const uint8_t *p = *pp;
   uint32_t c = *p;
   if (c < 0x80) { (*pp)++; return c; }
   if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      *pp += 2;
      return ((c & 0x1F) << 6) | (p[1] & 0x3F);
   }
   if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      *pp += 3;
      return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
   }
   if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      *pp += 4;
      return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
   }
   (*pp)++;
   return c;
}

float measureFontText(Font *f, int size, const char *text)
{
   if (!f->open || !text) return 0.0f;
   float fsize = (float)size;
   cellFontSetScalePixel(&f->font, fsize, fsize);

   float w = 0.0f;
   uint32_t prev = 0;
   const uint8_t *p = (const uint8_t *)text;
   while (*p) {
      uint32_t code = decodeUtf8(&p);
      CellFontGlyphMetrics metrics;
      if (cellFontGetCharGlyphMetrics(&f->font, code, &metrics) == CELL_OK)
         w += quantAdvance(f, metrics.Horizontal.advance) + fontKern(f, prev, code);
      else
         w += fsize;
      prev = code;
   }
   return w;
}

float measureFontChar(Font *f, int size, uint32_t code)
{
   float fsize = (float)size;
   cellFontSetScalePixel(&f->font, fsize, fsize);
   CellFontGlyphMetrics metrics;
   if (cellFontGetCharGlyphMetrics(&f->font, code, &metrics) == CELL_OK)
      return quantAdvance(f, metrics.Horizontal.advance);
   return fsize;
}

// inline style tags: {i} {b} {color=#rrggbb} (+ {{ and [[ escapes)
// Lightweight inline markup for styled runs inside one string. Text is pre-parsed into
// styled glyph items so tag handling stays separate from the (already fiddly)
// wrapping/layout logic, which then just walks the item array.
typedef struct { uint32_t code; uint8_t italic; uint8_t bold; uint32_t color; } GlyphItem;

// Parses text into items[] (<= maxItems). baseColor is the default colour. Nested {i}/{b}
// are counted; {color} uses a small stack. Unknown tags ({w},{p},{size=..},...) are skipped.
// rawMode disables all of the above - every byte (including '{'/'[') is literal content, for
// text that comes from outside the app (file contents, filenames, ...) rather than authored UI copy.
static int parseStyledText(const char *text, uint32_t baseColor, GlyphItem *items, int maxItems, int rawMode)
{
   int italic = 0, bold = 0;
   uint32_t colorStack[8];
   int colorTop = 0;
   colorStack[0] = baseColor;

   int n = 0;
   const uint8_t *p = (const uint8_t *)text;
   while (*p && n < maxItems) {
      if (!rawMode && p[0] == '{' && p[1] == '{') {   // literal brace
         items[n].code = '{'; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p += 2; continue;
      }
      if (!rawMode && p[0] == '[' && p[1] == '[') {   // literal bracket
         items[n].code = '['; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p += 2; continue;
      }
      if (!rawMode && p[0] == '{') {                  // a tag
         const uint8_t *q = p + 1;
         while (*q && *q != '}') q++;
         int len = (int)(q - (p + 1));
         const char *t = (const char *)(p + 1);
         if      (len == 1 && t[0] == 'i') italic++;
         else if (len == 2 && t[0] == '/' && t[1] == 'i') { if (italic > 0) italic--; }
         else if (len == 1 && t[0] == 'b') bold++;
         else if (len == 2 && t[0] == '/' && t[1] == 'b') { if (bold > 0) bold--; }
         else if (len >= 7 && strncmp(t, "color=#", 7) == 0) {
            // colour literal parsing lives in colors.h (shared with manifest parsing etc.)
            if (colorTop < 7) { colorTop++; colorStack[colorTop] = parseColorSpan(t + 7, len - 7, colorStack[colorTop - 1]); }
         }
         else if (len == 6 && strncmp(t, "/color", 6) == 0) { if (colorTop > 0) colorTop--; }
         // other tags carry no glyph: skip
         p = (*q == '}') ? q + 1 : q;
         continue;
      }
      if (p[0] == '\n') {
         items[n].code = '\n'; items[n].italic = (uint8_t)(italic>0); items[n].bold = (uint8_t)(bold>0); items[n].color = colorStack[colorTop]; n++; p++; continue;
      }
      uint32_t code = decodeUtf8(&p);
      items[n].code = code; items[n].italic = (uint8_t)(italic > 0); items[n].bold = (uint8_t)(bold > 0); items[n].color = colorStack[colorTop];
      n++;
   }
   return n;
}

static float getGlyphAdvance(Font *f, uint32_t code, float fsize)
{
   CellFontGlyphMetrics m;
   if (cellFontGetCharGlyphMetrics(&f->font, code, &m) == CELL_OK) return quantAdvance(f, m.Horizontal.advance);
   return fsize;
}

// Source-over composite of one glyph pixel (coverage a, colour cr/cg/cb) onto the surface
// pixel d = [a, r, g, b]. A plain write when the destination is empty, so the common case
// (text over a cleared surface) costs one branch.
static inline void composePixel(uint8_t *d, int a, uint8_t cr, uint8_t cg, uint8_t cb)
{
   if (a <= 0) return;
   int da = d[0];
   if (da == 0 || a >= 255) { d[0] = (uint8_t)a; d[1] = cr; d[2] = cg; d[3] = cb; return; }
   int outA = a + da * (255 - a) / 255;
   if (outA <= 0) return;
   d[1] = (uint8_t)((cr * a + d[1] * da * (255 - a) / 255) / outA);
   d[2] = (uint8_t)((cg * a + d[2] * da * (255 - a) / 255) / outA);
   d[3] = (uint8_t)((cb * a + d[3] * da * (255 - a) / 255) / outA);
   d[0] = (uint8_t)outA;
}

// Blits a rendered glyph's alpha bitmap into the surface, src-over composited.
//   dx/dy     offset from the glyph's nominal position (used by the shadow pass)
//   argb      colour; its ALPHA scales the glyph coverage (semi-transparent shadows/tags)
//   overhang  synthetic-bold strength in px (0 = regular). Faithful to FreeType's
//             FT_Bitmap_Embolden (src/base/ftbitmap.c, 8-bit gray path): each output pixel
//             is the SATURATING SUM of itself plus the `overhang` ORIGINAL pixels to its
//             left (a max would leave antialiased stroke interiors semi-transparent and
//             render thin, uneven bold), and the output is `overhang` columns wider so the
//             rightmost stroke thickens too (FreeType widens the bitmap the same way).
//             Reading from the glyph's own untouched bitmap matches FreeType's
//             right-to-left in-place pass. The caller widens the pen advance separately.
// Clipped against the surface bounds; surfBase/surfW/surfH locate the glyph's origin.
static void blitGlyph(const CellFontImageTransInfo *ti, uint8_t *surfBase, int surfW, int surfH,
                      int dx, int dy, uint32_t argb, int overhang)
{
   const uint8_t *img = (const uint8_t *)ti->Image;
   int imgW = ti->imageWidth, imgH = ti->imageHeight;
   int imgBW = ti->imageWidthByte, surfBW = ti->surfWidthByte;
   uint8_t cr = (uint8_t)((argb >> 16) & 0xFF), cg = (uint8_t)((argb >> 8) & 0xFF), cb = (uint8_t)(argb & 0xFF);
   int alpha = (int)((argb >> 24) & 0xFF);
   int off = (int)((const uint8_t *)ti->Surface - surfBase);
   int gx = (off % surfBW) / 4, gy = off / surfBW;
   int outW = imgW + overhang;
   for (int iy = 0; iy < imgH; iy++) {
      int ty = gy + iy + dy;
      if (ty < 0 || ty >= surfH) continue;
      const uint8_t *srow = img + iy * imgBW;
      uint8_t *drow = surfBase + ty * surfBW;
      for (int ix = 0; ix < outW; ix++) {
         int tx = gx + ix + dx;
         if (tx < 0 || tx >= surfW) continue;
         int a = (ix < imgW) ? srow[ix] : 0;
         for (int k = 1; k <= overhang && a < 255; k++) {
            int sx = ix - k;
            if (sx < 0) break;
            if (sx < imgW) a += srow[sx];
         }
         if (a > 255) a = 255;
         if (alpha < 255) a = a * alpha / 255;
         composePixel(drow + tx * 4, a, cr, cg, cb);
      }
   }
}

// width of the word starting at items[j0] (up to the next space/newline), including
// kerning inside the word and for the (prevCode -> first glyph) pair. Used by the wrap
// decision: a line breaks when "line + space + word" would exceed the target width.
static float getWordWidth(Font *f, const GlyphItem *items, int nItems, int j0, uint32_t prevCode, float fsize)
{
   float w = 0.0f;
   uint32_t prev = prevCode;
   for (int j = j0; j < nItems && items[j].code != ' ' && items[j].code != '\n'; j++) {
      w += getGlyphAdvance(f, items[j].code, fsize) + fontKern(f, prev, items[j].code);
      prev = items[j].code;
   }
   return w;
}

// records where laid-out item `index` ends (pen x + which wrapped line it's on) for a typewriter
// reveal -- taken from the real render pass, so the map can never desync from the texture.
static void recordRevealItem(TextReveal *reveal, int index, float penX, int line)
{
   if (index < TEXT_REVEAL_MAX) { reveal->endX[index] = (int)(penX + 0.5f); reveal->line[index] = line; }
}

// The x offset of a laid-out line within the text block for a given alignment: 0 for LEFT,
// (blockW - lineW)/2 for CENTER, (blockW - lineW) for RIGHT. blockW = the longest line, so
// the longest line gets offset 0 and the block crops to it.
static float getLineStartOffset(int align, const float *lineW, float maxLineW, int lineCount, int li)
{
   if (!lineW || align == TEXT_ALIGN_LEFT || li < 0 || li >= lineCount) return 0.0f;
   float d = maxLineW - lineW[li]; if (d < 0.0f) d = 0.0f;
   return (align == TEXT_ALIGN_CENTER) ? d * 0.5f : d;
}

// rasterizes text into a CPU buffer. returns the actual content dimensions
// via outW/outH. surfW is the buffer row width (may be larger than content).
// caller must free the returned buffer. `end` (optional) receives where the
// final glyph landed (texture coords). `align` aligns each line within the block.
static uint8_t *rasterize(Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap,
                          const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode,
                          int *outW, int *outH, int *outSurfW)
{
   *outW = *outH = *outSurfW = 0;

   int sdx = 0, sdy = 0, hasShadow = 0;
   uint32_t shadowArgb = 0;
   if (shadow && (shadow->dx != 0 || shadow->dy != 0)) {
      sdx = shadow->dx; sdy = shadow->dy; shadowArgb = shadow->color; hasShadow = 1;
   }

   float fsize = (float)size;
   cellFontSetScalePixel(&f->font, fsize, fsize);
   cellFontBindRenderer(&f->font, &fontRenderer);

   // Parse inline style tags into glyph items; layout/render walk this array.
   int cap = (int)strlen(text) + 1;
   GlyphItem *items = (GlyphItem *)malloc(sizeof(GlyphItem) * cap);
   if (!items) return NULL;
   int nItems = parseStyledText(text, color, items, cap, rawMode);

   // Baseline + line pitch the way Ren'Py does it (renpy/text/ftfont.pyx): from the FreeType (hhea)
   // ascender/descender -- ascent = ceil(ascender*size/upm), descent = floor(descender*size/upm),
   // line height = ascent - descent (descent negative). cellFont's CellFontHorizontalLayout instead
   // uses the OS/2 usWin* metrics, which are correct for fonts where they match hhea (e.g. primer.ttf)
   // but over-tall for some OTFs (toony_loons.otf: win 1.273em vs hhea 0.985em -> the box stretched).
   // Fall back to the cellFont layout only when the SFNT metrics weren't parsed (e.g. system fonts).
   float lineH, baseY;
   if (f->unitsPerEm > 0)
   {
      float asc = ceilf((float)f->hheaAscent  * fsize / (float)f->unitsPerEm);
      float dsc = floorf((float)f->hheaDescent * fsize / (float)f->unitsPerEm);   // descender is negative
      baseY = asc;
      lineH = asc - dsc;
   }
   else
   {
      CellFontHorizontalLayout layout;
      cellFontGetHorizontalLayout(&f->font, &layout);
      lineH = layout.lineHeight;
      baseY = layout.baseLineY;
   }

   // count lines for height calculation (must mirror the wrap logic in the render loop;
   // kerning resets per line, since each laid-out line is measured independently)
   int lineCount = 1;
   if (wrap == TEXT_WRAP && maxWidth > 0) {
      float px = 0.0f;
      uint32_t prev = 0;
      for (int i = 0; i < nItems; i++) {
         uint32_t code = items[i].code;
         if (code == '\n') { lineCount++; px = 0.0f; prev = 0; continue; }
         float adv = getGlyphAdvance(f, code, fsize) + fontKern(f, prev, code);
         if (code == ' ') {
            float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
            if (px + adv + wordW > (float)maxWidth && px > 0.0f) { lineCount++; px = 0.0f; prev = 0; continue; }
         }
         px += adv;
         prev = code;
      }
   }

   if (reveal) { reveal->lineHeight = (int)(lineH + 0.5f); reveal->count = 0; }

   // For CENTER/RIGHT alignment, measure each line's width (mirroring the wrap logic above
   // and the render pass exactly -- getGlyphAdvance == the rendered pen step) so each line can be
   // offset within the block at draw time. The wrap DECISIONS still use the line-relative pen,
   // so breaks are identical; only the draw x is shifted.
   float *lineW = NULL; float maxLineW = 0.0f;
   if (align != TEXT_ALIGN_LEFT && wrap == TEXT_WRAP && maxWidth > 0 && lineCount > 0) {
      lineW = (float *)malloc(sizeof(float) * lineCount);
      if (lineW) {
         int li = 0; float px = 0.0f; uint32_t prev = 0;
         for (int i = 0; i < nItems && li < lineCount; i++) {
            uint32_t code = items[i].code;
            if (code == '\n') { lineW[li++] = px; px = 0.0f; prev = 0; continue; }
            float adv = getGlyphAdvance(f, code, fsize) + fontKern(f, prev, code);
            if (code == ' ') {
               float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
               if (px + adv + wordW > (float)maxWidth && px > 0.0f) { lineW[li++] = px; px = 0.0f; prev = 0; continue; }
            }
            px += adv; prev = code;
         }
         if (li < lineCount) lineW[li] = px;
         for (int k = 0; k < lineCount; k++) if (lineW[k] > maxLineW) maxLineW = lineW[k];
      }
   }

   int surfW = (maxWidth > 0) ? maxWidth : FONT_MAX_RENDER_W;
   int surfH = (int)(lineH * lineCount + fsize) + 4;
   if (hasShadow && sdy > 0) surfH += sdy;   // room for the shadow below the last line

   size_t bufSize64 = (size_t)surfW * (size_t)surfH * 4u;
   if (bufSize64 == 0 || bufSize64 > FONT_MAX_SURFACE_BYTES) { free(items); free(lineW); return NULL; }
   int bufSize = (int)bufSize64;
   uint8_t *buf = (uint8_t *)malloc(bufSize);
   if (!buf) { free(items); free(lineW); return NULL; }
   memset(buf, 0, bufSize);

   CellFontRenderSurface surf;
   cellFontRenderSurfaceInit(&surf, buf, surfW * 4, 4, surfW, surfH);
   cellFontRenderSurfaceSetScissor(&surf, 0, 0, surfW, surfH);
   cellFontSetupRenderScalePixel(&f->font, fsize, fsize);

   // glyph effect state: italic via the slant shear (0.0 == genuinely no slant, safe to
   // reset). Bold is synthetic emboldening at blit time (see blitGlyph) -- the renderer's
   // weight effect is never touched, so tag-free text renders exactly like the default
   // path and the shared font object is never perturbed (a weight reset once thinned all
   // following text).
   //
   // The text is drawn in up to two passes: an optional SHADOW pass (every glyph in the
   // shadow colour, offset by sdx/sdy) and then the text pass composited over it -- the
   // classic order, so a later glyph's shadow never darkens an earlier glyph.
   int maxX = 0;
   int inkMinY = surfH, inkMaxY = 0;   // union ink box over all (non-shadow) glyphs

   float ellipsisW = 0.0f;
   if (wrap == TEXT_NOWRAP_ELLIPSIS && maxWidth > 0) {
      for (int i = 0; i < 3; i++) ellipsisW += measureFontChar(f, size, '.');
      cellFontSetScalePixel(&f->font, fsize, fsize);
      cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
   }

   for (int pass = hasShadow ? 0 : 1; pass < 2; pass++) {
      int shadowPass = (pass == 0);
      int bdx = shadowPass ? sdx : 0;
      int bdy = shadowPass ? sdy : 0;
      int curItalic = 0;
      int lineIndex = 0;   // which wrapped line the pen is on (for the reveal map)
      float penX = 0.0f;   // line-relative pen (wrap/kern/advance all use this; offset added at draw)
      float lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, 0);   // this line's draw offset
      float penY = baseY;
      uint32_t prev = 0;   // previous glyph on this line (kerning context; resets per line)

      for (int i = 0; i < nItems; i++) {
         uint32_t code = items[i].code;

         if (code == '\n') {
            if (wrap == TEXT_WRAP) { penX = 0.0f; penY += lineH; prev = 0; lineIndex++; lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, lineIndex); if (penY + lineH > (float)surfH) break; }
            if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
            continue;
         }

         float kern = fontKern(f, prev, code);
         float advance = getGlyphAdvance(f, code, fsize) + kern;

         if (maxWidth > 0) {
            if (wrap == TEXT_WRAP) {
               if (code == ' ') {
                  float wordW = getWordWidth(f, items, nItems, i + 1, ' ', fsize);
                  if (penX + advance + wordW > (float)maxWidth && penX > 0.0f) {
                     penX = 0.0f; penY += lineH; prev = 0; lineIndex++;   // the space wraps to the next line
                     lineOffX = getLineStartOffset(align, lineW, maxLineW, lineCount, lineIndex);
                     if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
                     if (penY + lineH > (float)surfH) break;
                     continue;
                  }
               }
            } else if (wrap == TEXT_NOWRAP_ELLIPSIS) {
               if (penX + advance > (float)maxWidth - ellipsisW && i + 1 < nItems) {
                  if (curItalic) {
                     cellFontSetupRenderEffectSlant(&f->font, 0.0f); curItalic = 0;
                     cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
                  }
                  for (int d = 0; d < 3; d++) {
                     CellFontImageTransInfo ti;
                     CellFontGlyphMetrics dm;
                     if (cellFontRenderCharGlyphImage(&f->font, '.', &surf, penX + lineOffX, penY, &dm, &ti) == CELL_OK) {
                        blitGlyph(&ti, buf, surfW, surfH, bdx, bdy, shadowPass ? shadowArgb : color, 0);
                        penX += dm.Horizontal.advance;
                     }
                  }
                  int endX = (int)(penX + lineOffX + 0.5f); if (endX > maxX) maxX = endX;
                  break;
               }
            } else {
               if (penX + advance > (float)maxWidth) break;
            }
         }

         // apply italic slant only when it changes (bold is faux-bold at blit time)
         int wantItalic = items[i].italic;
         if (wantItalic != curItalic) {
            cellFontSetupRenderEffectSlant(&f->font, wantItalic ? 0.35f : 0.0f); curItalic = wantItalic;
            // some SDKs reset the render scale when an effect is set -- re-apply it
            cellFontSetupRenderScalePixel(&f->font, fsize, fsize);
         }

         penX += kern;   // kerning positions THIS glyph (already counted in `advance`)

         CellFontImageTransInfo transInfo;
         CellFontGlyphMetrics metrics;
         int ret = cellFontRenderCharGlyphImage(&f->font, code, &surf, penX + lineOffX, penY, &metrics, &transInfo);
         if (ret == CELL_OK) {
            // synthetic-bold strength = pixel size / 10 (the FreeType convention; 0 at
            // tiny sizes = no embolden), and the pen advance widens by the same amount.
            int overhang = items[i].bold ? ((int)(fsize + 0.5f)) / 10 : 0;
            blitGlyph(&transInfo, buf, surfW, surfH, bdx, bdy,
                      shadowPass ? shadowArgb : items[i].color, overhang);
            penX += quantAdvance(f, metrics.Horizontal.advance) + (float)overhang;
            int endX = (int)(penX + lineOffX + 0.5f); if (endX > maxX) maxX = endX;
            if (!shadowPass) {
               int goff = (int)((const uint8_t *)transInfo.Surface - buf);
               int gy = goff / transInfo.surfWidthByte;
               if (gy < inkMinY) inkMinY = gy;                                   // union ink box
               if (gy + transInfo.imageHeight > inkMaxY) inkMaxY = gy + transInfo.imageHeight;
               if (end) {
                  end->endX = endX;
                  end->endTop = gy;
                  end->endBottom = gy + transInfo.imageHeight;
                  // Line cell top = baseline - ascent. The glyph's ink top gy sits
                  // bearingY below the baseline, so baseline = gy + bearingY, and the
                  // cell top (the top edge of the line box, where a caller top-aligns an
                  // inline decoration) = gy + bearingY - baseLineY(ascent). Same units as gy.
                  end->lineTop = gy + (int)(metrics.Horizontal.bearingY + 0.5f) - (int)baseY;
                  end->valid = 1;
               }
            }
         } else {
            penX += fsize;
         }
         if (!shadowPass && reveal) recordRevealItem(reveal, i, penX, lineIndex);
         prev = code;
      }

      // reset slant so the next pass / other render calls aren't left slanted
      if (curItalic) cellFontSetupRenderEffectSlant(&f->font, 0.0f);
   }

   if (reveal) reveal->count = nItems < TEXT_REVEAL_MAX ? nItems : TEXT_REVEAL_MAX;

   // crop the blank rows above the first line's glyphs
   int skip = (int)baseY + 4;
   if (end && end->valid) {
      end->endTop -= skip;    if (end->endTop < 0) end->endTop = 0;
      end->endBottom -= skip; if (end->endBottom < 0) end->endBottom = 0;
      end->lineTop -= skip;   // NOT clamped: the cell top can legitimately be above row 0
      end->lineHeight = (int)(lineH + 0.5f);
      end->inkTop    = (inkMinY <= inkMaxY) ? inkMinY - skip : 0;  if (end->inkTop < 0) end->inkTop = 0;
      end->inkBottom = (inkMinY <= inkMaxY) ? inkMaxY - skip : 0;  if (end->inkBottom < 0) end->inkBottom = 0;
   }
   *outW = maxX + (hasShadow && sdx > 0 ? sdx : 0);
   if (*outW > surfW) *outW = surfW;
   *outH = surfH - skip;
   *outSurfW = surfW;

   free(items);
   free(lineW);

   if (*outW <= 0 || *outH <= 0) { free(buf); return NULL; }

   uint8_t *cropped = buf + skip * surfW * 4;
   memmove(buf, cropped, (*outH) * surfW * 4);

   return buf;
}

void freeTextTexture(TextTexture *tt)
{
   if (tt->valid) {
      finishGfx();               // never free VRAM the RSX may still be reading
      freeGfxTexture(&tt->tex);  // also zeroes tt->tex
      tt->valid = 0;
   }
   tt->slotW = 0;
   tt->slotH = 0;
}

static void renderImpl(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode);

void renderFont(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap)
{
   renderFontEx(tt, f, size, text, color, maxWidth, wrap, NULL, NULL);
}

void renderFontEx(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end)
{
   renderFontTyped(tt, f, size, text, color, maxWidth, wrap, shadow, end, NULL);
}

void renderFontAligned(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextAlign align)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, shadow, end, NULL, align, 0);
}

void renderFontTyped(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, shadow, end, reveal, TEXT_ALIGN_LEFT, 0);
}

void renderFontRaw(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap)
{
   renderImpl(tt, f, size, text, color, maxWidth, wrap, NULL, NULL, NULL, TEXT_ALIGN_LEFT, 1);
}

static void renderImpl(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal, int align, int rawMode)
{
   if (end) memset(end, 0, sizeof *end);
   if (reveal) reveal->count = 0;
   if (!f->open) return;

   // clear case: hide the texture but keep the slot for reuse (a permanently
   // empty label is released at teardown via freeTextTexture, not here, so we
   // don't stall the RSX clearing rows during scrolling).
   if (!text || !text[0]) {
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   int drawW, drawH, surfW;
   uint8_t *buf = rasterize(f, size, text, color, maxWidth, wrap, shadow, end, reveal, align, rawMode, &drawW, &drawH, &surfW);
   if (!buf) {
      // nothing rasterized (e.g. all-whitespace after wrapping): keep any
      // existing slot but show nothing.
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   // reuse the existing slot when the new content still fits its high-water size.
   if (tt->valid && drawW <= tt->slotW && drawH <= tt->slotH) {
      // overwrite in place -- updateGfxTexture clears stale pixels. finishGfx first: the RSX
      // may still be sampling this slot for a draw call queued from the previous frame.
      finishGfx();
      updateGfxTexture(tt->tex.offset, buf, drawW, drawH, surfW * 4, tt->slotW, tt->slotH);
      tt->tex.w = drawW;
      tt->tex.h = drawH;
      tt->tex.pitch = (tt->slotW * 4 + 63) & ~63;
      free(buf);
      return;
   }

   // need a bigger slot: release the old one (after the RSX drains) and allocate.
   freeTextTexture(tt);

   uint32_t offset = uploadGfxTexture(buf, drawW, drawH, surfW * 4);
   free(buf);
   if (offset == 0) {
      // upload failed (out of VRAM): leave the slot empty.
      tt->tex.w = 0;
      tt->tex.h = 0;
      return;
   }

   tt->tex.offset = offset;
   tt->tex.w = drawW;
   tt->tex.h = drawH;
   tt->tex.pitch = (drawW * 4 + 63) & ~63;
   tt->slotW = drawW;
   tt->slotH = drawH;
   tt->valid = 1;
}
