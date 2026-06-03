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
    char text[LABEL_MAX_TEXT];
} Label;

void initLabel(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap, const char *text);
void setLabelText(Label *l, const char *text);
void moveLabel(Label *l, int x, int y);
void drawLabel(Label *l);

// draws the label at a custom opacity (alpha 0-255), ignoring the label's own
// alpha. used for ghosted/dimmed rows (e.g. files marked for cut).
void drawLabelAlpha(Label *l, int alpha);

// moves then draws in one call - handy when a label is repositioned every frame.
static inline void drawLabelAt(Label *l, int x, int y) { moveLabel(l, x, y); drawLabel(l); }
