// label - mutable text rendered to texture
#include "ui/label.h"
#include "colors.h"

void labelInit(Label *l, Font *font, int x, int y, int width, int height, int size, uint32_t color, TextWrap wrap)
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
}

void labelSetText(Label *l, const char *text)
{
    if (strncmp(l->text, text, LABEL_MAX_TEXT) == 0) return;

    strncpy(l->text, text, LABEL_MAX_TEXT - 1);
    l->text[LABEL_MAX_TEXT - 1] = '\0';

    fontRender(&l->tt, l->font, l->size, l->text, l->color, l->width, l->wrap);
}

void labelDraw(Label *l)
{
    if (l->tt.tex.w > 0)
        gfxDrawTexture(l->x, l->y, l->tt.tex.w, l->tt.tex.h, l->tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}
