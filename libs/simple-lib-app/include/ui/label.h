#pragma once

// label - mutable text rendered to texture

#include "gfx.h"
#include "font.h"
#include <string.h>

#define LABEL_MAX_TEXT 256

typedef struct {
   TextTexture tt;
   Font *font;
   int x, y;
   int width, height;
   int size;
   uint32_t color;
   TextWrap wrap;
   int raw;   // skip {i}/{b}/{color=} style-tag parsing - see initLabelRaw
   char text[LABEL_MAX_TEXT];
} Label;

void initLabel(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap, const char *text);

// as initLabel, but the text is always rendered literally (renderFontRaw) - for content that
// comes from outside the app (file contents, filenames, ...), where a stray '{' or '[' must
// never be swallowed as (or mistaken for) markup.
void initLabelRaw(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap, const char *text);

void setLabelText(Label *l, const char *text);

// re-renders the label in a new colour (for a live theme switch). a no-op when the colour is
// unchanged; otherwise re-rasterises the current text so the change shows without new text.
void setLabelColor(Label *l, uint32_t color);

// releases the label's text-texture VRAM; the label can be reused afterwards
// (setLabelText re-renders it).
void freeLabel(Label *l);
void moveLabel(Label *l, int x, int y);

// drawing only reads the label, so a screen can hold its labels const in the draw
// path - which is where the rule against re-rasterising text is easiest to break.
void drawLabel(const Label *l);

// draws the label at a custom opacity (alpha 0-255), ignoring the label's own
// alpha. used for ghosted/dimmed rows (e.g. files marked for cut).
void drawLabelAlpha(const Label *l, int alpha);

// moves then draws in one call - handy when a label is repositioned every frame.
static inline void drawLabelAt(Label *l, int x, int y) { moveLabel(l, x, y); drawLabel(l); }

// the Y to draw a label at so its actual rendered glyph height is centred within a
// rowHeight-tall row starting at rowY - the font's line-box height isn't the label's point
// size, so this reads the real rendered texture height rather than assuming one. the +3 is a
// manual correction against the original mockup (glyph ink doesn't sit dead-centre in its line
// box).
static inline int getCenteredLabelY(const Label *l, int rowY, int rowHeight)
{
   return rowY + (rowHeight - l->tt.tex.h) / 2 + 3;
}
