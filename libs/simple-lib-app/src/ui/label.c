// label - mutable text rendered to texture
#include "ui/label.h"
#include "colors.h"
#include "string-utilities.h"
#include <string.h>

void initLabel(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap, const char *text)
{
    l->font = font;
    l->x = x;
    l->y = y;
    l->width = width;
    l->height = height;
    l->size = size;
    l->color = color;
    l->wrap = wrap;
    l->text[0] = '\0';
    if (text) setLabelText(l, text);
}

void setLabelText(Label *l, const char *text)
{
    if (strncmp(l->text, text, LABEL_MAX_TEXT) == 0) return;

    strCopy(l->text, LABEL_MAX_TEXT, text);

    renderFont(&l->tt, l->font, l->size, l->text, l->color, l->width, l->wrap);
}

void moveLabel(Label *l, int x, int y)
{
    l->x = x;
    l->y = y;
}

void drawLabelAlpha(Label *l, int alpha)
{
    if (l->tt.tex.w > 0) {
        uint32_t tint = ((uint32_t)(alpha & 0xFF) << 24) | 0x00FFFFFF;
        drawGfxTexture(l->x, l->y, l->tt.tex.w, l->tt.tex.h, l->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, tint, GFX_FILTER_NEAREST);
    }
}

void drawLabel(Label *l) { drawLabelAlpha(l, (int)(l->color >> 24)); }
