// breadcrumb - navigable path display
#include "ui/breadcrumb.h"
#include "colors.h"
#include <string.h>

void breadcrumbInit(Breadcrumb *b, Font *font, int x, int y, int w, int h, uint32_t bgColor, uint32_t borderColor, uint32_t textColor, uint32_t chevronColor, int borderRadius, int borderThickness, int fontSize)
{
    memset(b, 0, sizeof(*b));
    b->font = font;
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
    b->padding = 18;
    b->chevronGap = 14;
    b->fontSize = fontSize;
    b->bgColor = bgColor;
    b->borderColor = borderColor;
    b->textColor = textColor;
    b->chevronColor = chevronColor;
    b->borderRadius = borderRadius;
    b->borderThickness = borderThickness;
    b->dirty = 0;
}

static void rebuild(Breadcrumb *b)
{
    for (int i = 0; i < b->depth; i++) {
        b->segTex[i] = fontToTexture(b->w, AUTO, b->segments[i], b->font, b->fontSize, b->textColor, TEXT_NOWRAP);
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

    // background with border (single SDF draw)
    gfxFillRoundedRectangle(b->x, b->y, b->w, b->h, b->borderRadius, b->bgColor, b->borderThickness, b->borderColor);

    int centerY = b->y + (b->h + 1) / 2;
    int chevronSize = b->fontSize / 4;

    // draw segments with chevron separators
    int cx = b->x + b->padding;

    for (int i = 0; i < b->depth; i++) {
        // draw chevron before segment (except first)
        if (i > 0) {
            int chevronX = cx;
            gfxDrawLine(chevronX, centerY - chevronSize, chevronX + chevronSize, centerY, 2, b->chevronColor);
            gfxDrawLine(chevronX + chevronSize, centerY, chevronX, centerY + chevronSize, 2, b->chevronColor);
            cx += chevronSize + b->chevronGap;
        }

        // draw segment text
        if (b->segTex[i].w > 0) {
            int textY = centerY - b->segTex[i].h / 2;
            gfxDrawTexture(cx, textY, b->segTex[i].w, b->segTex[i].h, b->segTex[i], 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
            cx += b->segTex[i].w + b->chevronGap;
        }
    }
}

void breadcrumbTerm(Breadcrumb *b)
{
    b->depth = 0;
    b->dirty = 0;
}
