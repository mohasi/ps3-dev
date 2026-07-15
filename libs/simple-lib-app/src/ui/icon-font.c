// icon-font - draws icon-font glyphs as tight white textures, tinted at draw (see ui/icon-font.h). Glyph
// rasterisation lives in font.c (renderGlyphTexture, which owns the cellFont renderer); here we keep a
// shared cache of one texture per (id, size) so every widget drawing the same icon reuses one texture.
#include "ui/icon-font.h"
#include "gfx.h"
#include "dbg.h"   // logWarn on a cache-full / rasterise failure

// the embedded font bytes, generated into icon-data.c by sprite-packer's icons mode.
extern const unsigned char iconFontData[];
extern const unsigned int  iconFontDataSize;

static Font iconFont;
static int  loaded;

#define ICON_CACHE_MAX 128   // one entry per distinct (id, size); over-cache = the glyph silently won't draw
typedef struct { IconId id; int size; GfxTexture tex; int offsetX, offsetY; } IconCacheEntry;
static IconCacheEntry iconCache[ICON_CACHE_MAX];
static int            iconCacheCount;

int initIconFont(void)
{
   if (loaded) return 0;
   iconFont = openFontMemory(iconFontData, iconFontDataSize);
   if (!iconFont.open) return -1;
   loaded = 1;
   return 0;
}

void freeIconFont(void)
{
   if (!loaded) return;
   if (iconCacheCount > 0) {
      finishGfx();   // never free VRAM the RSX may still be reading
      for (int i = 0; i < iconCacheCount; i++) freeGfxTexture(&iconCache[i].tex);
      iconCacheCount = 0;
   }
   closeFont(&iconFont);
   loaded = 0;
}

// the shared cache entry for one glyph at one size: reuse a cached one, else rasterise into a free slot.
static const IconCacheEntry *getCachedGlyph(IconId id, int size)
{
   if (!loaded) return (const IconCacheEntry *)0;
   for (int i = 0; i < iconCacheCount; i++)
      if (iconCache[i].id == id && iconCache[i].size == size) return &iconCache[i];
   if (iconCacheCount >= ICON_CACHE_MAX) {
      logWarn("[icons] glyph cache full (%d), icon 0x%x @%d not drawn\n", ICON_CACHE_MAX, (unsigned)id, size);
      return (const IconCacheEntry *)0;
   }

   IconCacheEntry *entry = &iconCache[iconCacheCount];
   if (renderGlyphTexture(&iconFont, size, (uint32_t)id, &entry->tex, &entry->offsetX, &entry->offsetY) != 0) {
      logWarn("[icons] failed to rasterise glyph 0x%x @%d\n", (unsigned)id, size);
      return (const IconCacheEntry *)0;
   }
   entry->id = id;
   entry->size = size;
   iconCacheCount++;
   return entry;
}

void initIcon(Icon *icon, IconId id, int size)
{
   icon->id   = id;
   icon->size = size;
   const IconCacheEntry *entry = getCachedGlyph(id, size);
   icon->tex     = entry ? &entry->tex : (const GfxTexture *)0;
   icon->offsetX = entry ? entry->offsetX : 0;
   icon->offsetY = entry ? entry->offsetY : 0;
}

// draw the glyph at its natural size; (x, y) is the em-box top-left and the glyph's own metrics place it
// within, so same-size icons drawn at one anchor line up as the font intends. tinted, no scaling.
void drawIconAlpha(Icon *icon, int x, int y, uint32_t color, int alpha)
{
   if (!icon->tex || icon->tex->w <= 0 || icon->tex->h <= 0) return;
   uint32_t tint = ((uint32_t)(alpha & 0xFF) << 24) | (color & 0x00FFFFFF);
   drawGfxTexture(x + icon->offsetX, y + icon->offsetY, icon->tex->w, icon->tex->h, *icon->tex,
                  0.0f, 0.0f, 1.0f, 1.0f, tint, GFX_FILTER_LINEAR);
}

void drawIcon(Icon *icon, int x, int y, uint32_t color) { drawIconAlpha(icon, x, y, color, (int)(color >> 24)); }

void drawIconCentered(Icon *icon, int centerX, int y, uint32_t color)
{
   if (!icon->tex) return;   // subtract offsetX so the ink (not the em box) lands centred on centerX
   drawIcon(icon, centerX - icon->tex->w / 2 - icon->offsetX, y, color);
}

void freeIcon(Icon *icon)
{
   icon->tex = (const GfxTexture *)0;   // textures are owned by the shared cache (freed in freeIconFont)
}
