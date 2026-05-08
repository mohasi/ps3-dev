#pragma once

// label - mutable text rendered to texture

#include "gfx.h"
#include "font.h"
#include <string.h>

#define LABEL_MAX_TEXT 256

typedef struct {
    GfxTexture tex;
    Font *font;
    int x, y;
    int width, height;
    int size;
    uint32_t color;
    TextWrap wrap;
    char text[LABEL_MAX_TEXT];
} Label;

void labelInit(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap);
void labelSetText(Label *l, const char *text);
void labelDraw(Label *l);
