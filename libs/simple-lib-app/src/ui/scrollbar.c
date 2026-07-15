// scrollbar - flat track + thumb, proportional height/position math
#include "ui/scrollbar.h"

void initScrollbar(Scrollbar *sb, int trackX, int trackY, int trackW, int trackH,
                   int thumbWidth, int minThumbHeight, uint32_t trackColor, uint32_t thumbColor)
{
   sb->x = trackX + (trackW - thumbWidth) / 2;
   sb->w = thumbWidth;
   sb->trackY = trackY;
   sb->trackH = trackH;
   sb->minThumbHeight = minThumbHeight;
   sb->trackColor = trackColor;
   sb->thumbColor = thumbColor;
}

void rethemeScrollbar(Scrollbar *sb, uint32_t trackColor, uint32_t thumbColor)
{
   sb->trackColor = trackColor;
   sb->thumbColor = thumbColor;
}

void drawScrollbar(Scrollbar *sb, int totalItems, int visibleItems, int scrollTop)
{
   if (totalItems <= visibleItems) return;

   fillGfxRectangle(sb->x, sb->trackY, sb->w, sb->trackH, sb->trackColor);

   int height = (int)((int64_t)sb->trackH * visibleItems / totalItems);
   if (height < sb->minThumbHeight) height = sb->minThumbHeight;

   int maxScroll = totalItems - visibleItems;
   int y = sb->trackY + (sb->trackH - height) * scrollTop / maxScroll;
   fillGfxRectangle(sb->x, y, sb->w, height, sb->thumbColor);
}
