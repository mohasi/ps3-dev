// breadcrumb - navigable path display
#include "ui/breadcrumb.h"
#include "colors.h"
#include "string-utilities.h"
#include <string.h>

static const int BREADCRUMB_CHEVRON_GAP = 14;

void initBreadcrumb(Breadcrumb *b, Font *font, int x, int y, uint32_t textColor, GfxTexture chevronTex, SpriteRegion chevronSrc, int fontSize)
{
   memset(b, 0, sizeof(*b));
   b->font = font;
   b->x = x;
   b->y = y;
   b->fontSize = fontSize;
   b->textColor = textColor;
   b->chevronTex = chevronTex;
   b->chevronW = chevronSrc.w;
   b->chevronH = chevronSrc.h;
   if (chevronTex.w > 0 && chevronTex.h > 0) {
      b->chevronU0 = (float)chevronSrc.x / (float)chevronTex.w;
      b->chevronV0 = (float)chevronSrc.y / (float)chevronTex.h;
      b->chevronU1 = (float)(chevronSrc.x + chevronSrc.w) / (float)chevronTex.w;
      b->chevronV1 = (float)(chevronSrc.y + chevronSrc.h) / (float)chevronTex.h;
   }
   setBreadcrumbPath(b, "/");
}

static void rebuild(Breadcrumb *b)
{
   for (int i = 0; i < b->depth; i++) {
      renderFont(&b->segTex[i], b->font, b->fontSize, b->segments[i], b->textColor, AUTO, TEXT_NOWRAP);
   }
   b->dirty = 0;
}

void setBreadcrumbPath(Breadcrumb *b, const char *path)
{
   b->depth = 0;

   char buf[512];
   strCopy(buf, sizeof buf, path);

   char *p = buf;
   if (*p == '/') p++;

   // root shows as "/". everything below root shows bare segments
   // ("dev_hdd0" > "PS3" > ...), no leading slash on the first.
   if (!*p) {
      strCopy(b->segments[0], BREADCRUMB_MAX_NAME, "/");
      b->depth = 1;
   }

   while (*p && b->depth < BREADCRUMB_MAX_DEPTH) {
      char *slash = p;
      while (*slash && *slash != '/') slash++;
      char saved = *slash;
      *slash = '\0';
      if (p[0] != '\0') {
         strCopy(b->segments[b->depth], BREADCRUMB_MAX_NAME, p);
         b->depth++;
      }
      if (saved) p = slash + 1;
      else break;
   }
   b->dirty = 1;
}

void drawBreadcrumb(Breadcrumb *b)
{
   if (b->dirty) rebuild(b);

   int cx = b->x;

   for (int i = 0; i < b->depth; i++) {
      if (i > 0 && b->chevronW > 0) {
         int chevronY = b->y + (b->segTex[0].tex.h - b->chevronH) / 2;
         drawGfxTexture(cx, chevronY, b->chevronW, b->chevronH, b->chevronTex, b->chevronU0, b->chevronV0, b->chevronU1, b->chevronV1, COLOR_WHITE, GFX_FILTER_LINEAR);
         cx += b->chevronW + BREADCRUMB_CHEVRON_GAP;
      }

      if (b->segTex[i].tex.w > 0) {
         drawGfxTexture(cx, b->y, b->segTex[i].tex.w, b->segTex[i].tex.h, b->segTex[i].tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
         cx += b->segTex[i].tex.w + BREADCRUMB_CHEVRON_GAP;
      }
   }
}

void termBreadcrumb(Breadcrumb *b)
{
   for (int i = 0; i < BREADCRUMB_MAX_DEPTH; i++)
      freeTextTexture(&b->segTex[i]);
   b->depth = 0;
   b->dirty = 0;
}
