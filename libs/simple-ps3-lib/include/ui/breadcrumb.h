#pragma once

// breadcrumb - navigable path display

#include "gfx.h"
#include "font.h"
#include <stdint.h>

#define BREADCRUMB_MAX_DEPTH 16
#define BREADCRUMB_MAX_NAME  64

typedef struct {
    char segments[BREADCRUMB_MAX_DEPTH][BREADCRUMB_MAX_NAME];
    GfxTexture segTex[BREADCRUMB_MAX_DEPTH];
    int depth;
    int x, y, w, h;
    int padding;
    int fontSize;
    int chevronGap;
    uint32_t bgColor;
    uint32_t borderColor;
    uint32_t textColor;
    uint32_t chevronColor;
    int borderRadius;
    int borderThickness;
    Font *font;
    int dirty;
} Breadcrumb;

void breadcrumbInit(Breadcrumb *b, Font *font, int x, int y, int w, int h, uint32_t bgColor, uint32_t borderColor, uint32_t textColor, uint32_t chevronColor, int borderRadius, int borderThickness, int fontSize);
void breadcrumbPush(Breadcrumb *b, const char *name);
void breadcrumbPop(Breadcrumb *b);
void breadcrumbClear(Breadcrumb *b);
void breadcrumbDraw(Breadcrumb *b);
void breadcrumbTerm(Breadcrumb *b);
