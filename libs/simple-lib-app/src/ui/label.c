// label - mutable text rendered to texture
#include "ui/label.h"
#include "colors.h"

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

    strncpy(l->text, text, LABEL_MAX_TEXT - 1);
    l->text[LABEL_MAX_TEXT - 1] = '\0';

    renderFont(&l->tt, l->font, l->size, l->text, l->color, l->width, l->wrap);
}

void moveLabel(Label *l, int x, int y)
{
    l->x = x;
    l->y = y;
}

void drawLabel(Label *l)
{
    if (l->tt.tex.w > 0)
        drawGfxTexture(l->x, l->y, l->tt.tex.w, l->tt.tex.h, l->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}
