// breadcrumb - navigable path display
#include "ui/breadcrumb.h"
#include "colors.h"
#include "string-utilities.h"
#include <string.h>

static const int BREADCRUMB_SEPARATOR_GAP = 14;

// the "›" drawn between segments, rendered dimmer than the text so it reads as chrome, not a name.
static void renderSeparator(Breadcrumb *b)
{
   renderFont(&b->separatorTex, b->font, b->fontSize, "\xE2\x80\xBA", (b->textColor & 0x00FFFFFF) | 0x80000000, AUTO, TEXT_NOWRAP);
}

void initBreadcrumb(Breadcrumb *b, Font *font, int x, int y, uint32_t textColor, int fontSize)
{
   memset(b, 0, sizeof(*b));
   b->font = font;
   b->x = x;
   b->y = y;
   b->fontSize = fontSize;
   b->textColor = textColor;
   renderSeparator(b);   // never changes with the path, so render it once
   setBreadcrumbPath(b, "/");
}

void rethemeBreadcrumb(Breadcrumb *b, uint32_t textColor)
{
   b->textColor = textColor;
   freeTextTexture(&b->separatorTex);
   renderSeparator(b);
   b->dirty = 1;   // segments re-render in the new colour on the next draw
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
      if (i > 0 && b->separatorTex.tex.w > 0) {
         drawGfxTexture(cx, b->y - 2, b->separatorTex.tex.w, b->separatorTex.tex.h, b->separatorTex.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
         cx += b->separatorTex.tex.w + BREADCRUMB_SEPARATOR_GAP;
      }

      if (b->segTex[i].tex.w > 0) {
         drawGfxTexture(cx, b->y, b->segTex[i].tex.w, b->segTex[i].tex.h, b->segTex[i].tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
         cx += b->segTex[i].tex.w + BREADCRUMB_SEPARATOR_GAP;
      }
   }
}

void termBreadcrumb(Breadcrumb *b)
{
   for (int i = 0; i < BREADCRUMB_MAX_DEPTH; i++)
      freeTextTexture(&b->segTex[i]);
   freeTextTexture(&b->separatorTex);
   b->depth = 0;
   b->dirty = 0;
}
