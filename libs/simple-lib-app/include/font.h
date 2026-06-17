#pragma once

// font - truetype font rendering via libfont/freetype

#include <cell/font.h>
#include <stdint.h>
#include "gfx.h"

// system font identifiers
#define FONT_POP       0
#define FONT_GOTHIC_JP 1
#define FONT_SANS      2
#define FONT_SERIF     3

// glyph-advance model. Classic SDL-era text stacks round every advance UP to whole
// pixels (FreeType's FT_CEIL) and apply kerning between glyph pairs; modern stacks use
// fractional advances without kerning. Emulating a UI authored for the classic metrics
// at design resolution, drawn at scale s, uses GRID_CEIL with grid = s (advances snap up
// to the design-pixel grid, kerning snaps down) -- line breaks then land exactly where
// the original did.
typedef enum {
   FONT_METRICS_NATIVE = 0,   // fractional advances, no kerning (default)
   FONT_METRICS_GRID_CEIL     // advance = ceil(adv/grid)*grid + kerning floored to grid
} FontMetricsMode;

typedef struct {
   CellFont font;
   int      open;
   void    *buffer;      // backing memory for openFontFile (cellFont keeps using it); NULL for system fonts
   int      metricsMode; // FontMetricsMode (default FONT_METRICS_NATIVE)
   float    metricsGrid; // grid in output px for FONT_METRICS_GRID_CEIL
   // SFNT hhea/head metrics, parsed from the font file at open. Ren'Py's line height is the FreeType
   // ascender - descender (hhea), NOT cellFont's OS/2-Windows-metric lineHeight (which over-spaces some
   // OTF fonts). unitsPerEm == 0 => unavailable (system fonts) -> fall back to the cellFont layout.
   int      unitsPerEm;
   int      hheaAscent;   // hhea.ascender  (font design units; positive)
   int      hheaDescent;  // hhea.descender (font design units; negative)
} Font;

// Selects the advance model used by measuring and rendering (see FontMetricsMode).
void setFontMetrics(Font *f, FontMetricsMode mode, float grid);

typedef enum {
   TEXT_WRAP,
   TEXT_NOWRAP,
   TEXT_NOWRAP_ELLIPSIS
} TextWrap;

// Horizontal alignment of each laid-out line within the text block (block width = the
// longest line). LEFT is the default for renderFont*/renderFontEx; CENTER/RIGHT match a
// text style's text_align (e.g. Ren'Py prompt_text.text_align 0.5 + layout "subtitle").
typedef enum {
   TEXT_ALIGN_LEFT = 0,
   TEXT_ALIGN_CENTER,
   TEXT_ALIGN_RIGHT
} TextAlign;

// reusable text texture - owns a VRAM slot that grows as needed
typedef struct {
   GfxTexture tex;   // current texture (w/h = actual content size)
   int slotW, slotH; // allocated slot dimensions (high-water mark)
   int valid;        // 1 while tex holds a live VRAM allocation owned by this slot
} TextTexture;

int   initFont(void);
void  termFont(void);
Font  openSystemFont(int type);
Font  openFontFile(const char *path);
// Opens a TTF/OTF directly from a memory buffer (no temp file). Makes its own copy, so
// the caller may free buf immediately; closeFont releases the copy.
Font  openFontMemory(const void *buf, uint32_t size);
void  closeFont(Font *f);
float measureFontText(Font *f, int size, const char *text);
float measureFontChar(Font *f, int size, uint32_t code);

// drop shadow for renderFontEx: the full text is drawn once in `color` offset by
// (dx, dy) output pixels, then again on top in the text colour -- the classic
// two-pass shadow, so later glyphs' shadows never darken earlier glyphs.
typedef struct {
   int      dx, dy;   // offset in output pixels (positive = right/down)
   uint32_t color;    // 0xAARRGGBB (alpha is used)
} TextShadow;

// renders text into a reusable texture slot. reuses existing VRAM when the
// rendered size fits, grows the slot if needed. clears stale pixels
// automatically. pass NULL or empty text to clear the texture.
void renderFont(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap);

// where the laid-out text ENDS, reported by renderFontEx -- for placing inline
// decorations (carets, click-to-continue icons) right after the final glyph.
// All values are texture-relative pixels.
typedef struct {
   int endX;       // pen x after the final glyph (its advance included)
   int endTop;     // top of the final glyph's ink box
   int endBottom;  // bottom of the final glyph's ink box (~the baseline for ., a, m, ...)
   int lineTop;    // top of the final glyph's LINE CELL (= baseline - ascent); where an
               // inline widget's top sits (may be negative if above the cropped texture)
   int lineHeight; // the laid-out line pitch in texture px (the font's line box height)
   int inkTop;     // top of the UNION ink box over all glyphs (texture px; >= 0)
   int inkBottom;  // bottom of the union ink box over all glyphs (texture px)
   int valid;      // 1 when at least one glyph was laid out
} TextEnd;

// Per-item layout positions for a typewriter reveal: where each laid-out item (char/space/
// newline) ENDS, in the rendered texture's pixel space. The text is rendered ONCE in full;
// a caller then shows the first N items by clipping the texture (no per-frame re-rasterising).
#define TEXT_REVEAL_MAX 1024
typedef struct {
   int count;                    // laid-out items (capped at TEXT_REVEAL_MAX)
   int lineHeight;               // line height in texture px
   int endX[TEXT_REVEAL_MAX];    // pen x after item i (texture coords)
   int line[TEXT_REVEAL_MAX];    // which wrapped line item i sits on (0 = first); top = line*lineHeight
} TextReveal;

// renderFont with an optional drop shadow (NULL or a zero offset = none) and an
// optional layout report (NULL if not wanted). Lines are left-aligned.
void renderFontEx(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end);

// As renderFontEx, but each laid-out line is aligned (LEFT/CENTER/RIGHT) within the text
// block (block width = the longest line) -- a text style's text_align. CENTER/RIGHT only
// affects multi-line / wrapped text; a single line is unchanged.
void renderFontAligned(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextAlign align);

// As renderFontEx, but also reports per-item end positions into `reveal` (which line, and
// the pen x), so a caller can later show the first N characters by clipping the rendered
// texture -- e.g. a typewriter effect, or a text cursor/selection. Renders the text in full.
void renderFontTyped(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap, const TextShadow *shadow, TextEnd *end, TextReveal *reveal);

// releases the VRAM slot a TextTexture owns (if any) and resets it so it can be
// re-rendered later. Waits for the RSX first, so it is safe to call mid-frame.
// Owners call this in their term()/teardown to avoid leaking text VRAM.
void freeTextTexture(TextTexture *tt);
