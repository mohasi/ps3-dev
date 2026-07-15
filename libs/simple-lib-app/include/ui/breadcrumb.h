#pragma once

// breadcrumb - navigable path display

#include "gfx.h"
#include "font.h"
#include <stdint.h>

#define BREADCRUMB_MAX_DEPTH 16
#define BREADCRUMB_MAX_NAME  64

typedef struct {
   char segments[BREADCRUMB_MAX_DEPTH][BREADCRUMB_MAX_NAME];
   TextTexture segTex[BREADCRUMB_MAX_DEPTH];
   TextTexture separatorTex;   // the ">" drawn between segments (metro style, no sprite)
   int depth;
   int x, y;
   int fontSize;
   uint32_t textColor;
   Font *font;
   int dirty;
} Breadcrumb;

void initBreadcrumb(Breadcrumb *b, Font *font, int x, int y, uint32_t textColor, int fontSize);
void rethemeBreadcrumb(Breadcrumb *b, uint32_t textColor);   // re-render in a new colour (live theme switch)
void setBreadcrumbPath(Breadcrumb *b, const char *path);
void drawBreadcrumb(Breadcrumb *b);
void termBreadcrumb(Breadcrumb *b);
