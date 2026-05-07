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

#define FONT_MAX_RENDER_W 1024

typedef struct {
    CellFont font;
    int      open;
} Font;

typedef enum {
    TEXT_WRAP,
    TEXT_NOWRAP,
    TEXT_NOWRAP_ELLIPSIS
} TextWrap;

#define TEXT_AUTOSIZE 0

int   fontInit(void);
void  fontTerm(void);
Font  fontOpenSystem(int type);
Font  fontOpenFile(const char *path);
void  fontClose(Font *f);
float fontMeasureText(Font *f, int size, const char *text);
float fontMeasureChar(Font *f, int size, uint32_t code);
void  fontDraw(int x, int y, int width, int height, const char *text, Font *f, int size, uint32_t color, TextWrap wrap);
GfxTexture fontToTexture(int width, int height, const char *text, Font *f, int size, uint32_t color, TextWrap wrap);
