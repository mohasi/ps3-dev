// scrollbar - pill thumb + track, proportional height/position math
#include "ui/scrollbar.h"

void initScrollbar(Scrollbar *sb, GfxTexture sprites, int trackX, int trackY, int trackW, int trackH,
                    SpriteRegion thumbSprite, int thumbWidth, int cap, int minThumbHeight)
{
   int thumbX = trackX + (trackW - thumbWidth) / 2;
   initNineSlice(&sb->thumb, sprites, thumbX, trackY, thumbWidth, trackH, thumbSprite, cap, cap);
   sb->trackY = trackY;
   sb->trackH = trackH;
   sb->minThumbHeight = minThumbHeight;
}

void drawScrollbar(Scrollbar *sb, int totalItems, int visibleItems, int scrollTop)
{
   if (totalItems <= visibleItems) return;

   int height = (int)((int64_t)sb->trackH * visibleItems / totalItems);
   if (height < sb->minThumbHeight) height = sb->minThumbHeight;

   int maxScroll = totalItems - visibleItems;
   int y = sb->trackY + (sb->trackH - height) * scrollTop / maxScroll;

   sb->thumb.h = height;
   moveNineSlice(&sb->thumb, sb->thumb.x, y);
   drawNineSlice(&sb->thumb);
}
