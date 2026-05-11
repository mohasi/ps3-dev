#pragma once

// breadcrumb - navigable path display

#include "gfx.h"
#include "font.h"
#include <stdint.h>

#define BREADCRUMB_MAX_DEPTH 16
#define BREADCRUMB_MAX_NAME  64
#define BREADCRUMB_CHEVRON_GAP 14

typedef struct {
    char segments[BREADCRUMB_MAX_DEPTH][BREADCRUMB_MAX_NAME];
    TextTexture segTex[BREADCRUMB_MAX_DEPTH];
    GfxTexture chevronTex;
    float chevronU0, chevronV0, chevronU1, chevronV1;
    int chevronW, chevronH;
    int depth;
    int x, y;
    int fontSize;
    uint32_t textColor;
    Font *font;
    int dirty;
} Breadcrumb;

void breadcrumbInit(Breadcrumb *b, Font *font, int x, int y, uint32_t textColor, GfxTexture chevronTex, int chevronSrcX, int chevronSrcY, int chevronSrcW, int chevronSrcH, int fontSize);
void breadcrumbPush(Breadcrumb *b, const char *name);
void breadcrumbPop(Breadcrumb *b);
void breadcrumbClear(Breadcrumb *b);
void breadcrumbDraw(Breadcrumb *b);
void breadcrumbTerm(Breadcrumb *b);
