// breadcrumb - navigable path display
#include "ui/breadcrumb.h"
#include "colors.h"
#include <string.h>

void breadcrumbInit(Breadcrumb *b, Font *font, int x, int y, uint32_t textColor, GfxTexture chevronTex, int chevronSrcX, int chevronSrcY, int chevronSrcW, int chevronSrcH, int fontSize)
{
    memset(b, 0, sizeof(*b));
    b->font = font;
    b->x = x;
    b->y = y;
    b->fontSize = fontSize;
    b->textColor = textColor;
    b->chevronTex = chevronTex;
    b->chevronW = chevronSrcW;
    b->chevronH = chevronSrcH;
    b->chevronU0 = (float)chevronSrcX / (float)chevronTex.w;
    b->chevronV0 = (float)chevronSrcY / (float)chevronTex.h;
    b->chevronU1 = (float)(chevronSrcX + chevronSrcW) / (float)chevronTex.w;
    b->chevronV1 = (float)(chevronSrcY + chevronSrcH) / (float)chevronTex.h;
}

static void rebuild(Breadcrumb *b)
{
    for (int i = 0; i < b->depth; i++) {
        fontRender(&b->segTex[i], b->font, b->fontSize, b->segments[i], b->textColor, AUTO, TEXT_NOWRAP);
    }
    b->dirty = 0;
}

void breadcrumbPush(Breadcrumb *b, const char *name)
{
    if (b->depth >= BREADCRUMB_MAX_DEPTH) return;
    strncpy(b->segments[b->depth], name, BREADCRUMB_MAX_NAME - 1);
    b->segments[b->depth][BREADCRUMB_MAX_NAME - 1] = '\0';
    b->depth++;
    b->dirty = 1;
}

void breadcrumbPop(Breadcrumb *b)
{
    if (b->depth <= 0) return;
    b->depth--;
    b->dirty = 1;
}

void breadcrumbClear(Breadcrumb *b)
{
    b->depth = 0;
    b->dirty = 1;
}

void breadcrumbDraw(Breadcrumb *b)
{
    if (b->dirty) rebuild(b);

    int cx = b->x;

    for (int i = 0; i < b->depth; i++) {
        if (i > 0 && b->chevronW > 0) {
            int chevronY = b->y + (b->segTex[0].tex.h - b->chevronH) / 2;
            gfxDrawTexture(cx, chevronY, b->chevronW, b->chevronH, b->chevronTex, b->chevronU0, b->chevronV0, b->chevronU1, b->chevronV1, COLOR_WHITE, GFX_FILTER_LINEAR);
            cx += b->chevronW + BREADCRUMB_CHEVRON_GAP;
        }

        if (b->segTex[i].tex.w > 0) {
            gfxDrawTexture(cx, b->y, b->segTex[i].tex.w, b->segTex[i].tex.h, b->segTex[i].tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
            cx += b->segTex[i].tex.w + BREADCRUMB_CHEVRON_GAP;
        }
    }
}

void breadcrumbTerm(Breadcrumb *b)
{
    b->depth = 0;
    b->dirty = 0;
}
