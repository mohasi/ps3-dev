#pragma once

// console-glyphs - decodes the PS3's own controller-button glyphs from the system font
// (/dev_flash/vsh/resource/imagefont.bin) into textures at runtime, so any app can render native XMB-style
// button hints without shipping its own art. The glyphs are zlib-compressed paletted bitmaps; decoding
// reuses the vendored tinfl inflate (see zip.h). Load once at startup, then look one up by id.
//
// requires vsh-class filesystem privilege to read /dev_flash (the NPDRM apps in this repo have it).

#include "gfx.h"   // GfxTexture
#include "ui/image.h"

typedef enum {
   GLYPH_CROSS, GLYPH_CIRCLE, GLYPH_SQUARE, GLYPH_TRIANGLE,
   GLYPH_L1, GLYPH_R1, GLYPH_L2, GLYPH_R2,
   GLYPH_SELECT, GLYPH_START, GLYPH_R3,
   GLYPH_DPAD_UP, GLYPH_DPAD_DOWN, GLYPH_DPAD_LEFT, GLYPH_DPAD_RIGHT,
   GLYPH_COUNT
} ConsoleGlyph;

// decode the button set once (idempotent). 0 if at least one glyph decoded, -1 if the font is unreadable.
int  loadConsoleGlyphs(void);

// the glyph's texture (its .w/.h are the glyph's pixel size). a zeroed texture (.offset == 0) if not loaded.
GfxTexture getConsoleGlyph(ConsoleGlyph glyph);

void freeConsoleGlyphs(void);

// builds an Image from a glyph, scaled to height with aspect preserved (same approach as
// button-hints.c's addButtonHint) - the common way callers turn a glyph into a footer icon.
static inline void initGlyphIcon(Image *icon, ConsoleGlyph glyph, int height)
{
   GfxTexture texture = getConsoleGlyph(glyph);
   int width = texture.h > 0 ? texture.w * height / texture.h : height;
   SpriteRegion wholeTexture = {0};
   initImage(icon, texture, 0, 0, width, height, wholeTexture, GFX_FILTER_LINEAR);
}
