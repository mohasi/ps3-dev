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
    void    *buffer;  // backing memory for openFontFile (cellFont keeps using it); NULL for system fonts
} Font;

typedef enum {
    TEXT_WRAP,
    TEXT_NOWRAP,
    TEXT_NOWRAP_ELLIPSIS
} TextWrap;

// reusable text texture - owns a VRAM slot that grows as needed
typedef struct {
    GfxTexture tex;   // current texture (w/h = actual content size)
    int slotW, slotH; // allocated slot dimensions (high-water mark)
} TextTexture;

int   initFont(void);
void  termFont(void);
Font  openSystemFont(int type);
Font  openFontFile(const char *path);
void  closeFont(Font *f);
float measureFontText(Font *f, int size, const char *text);
float measureFontChar(Font *f, int size, uint32_t code);

// renders text into a reusable texture slot. reuses existing VRAM when the
// rendered size fits, grows the slot if needed. clears stale pixels
// automatically. pass NULL or empty text to clear the texture.
void renderFont(TextTexture *tt, Font *f, int size, const char *text, uint32_t color, int maxWidth, TextWrap wrap);
