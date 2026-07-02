// dialog-panel - scrim + centered panel body + border
#include "ui/dialog-panel.h"
#include "gfx.h"

#define COLOR_SCRIM 0xC8000000u   // black at 200/255

void initDialogPanel(DialogPanel *panel, GfxTexture sprites, int w, int h, uint32_t bgColor,
                      SpriteRegion borderSprite, int cap)
{
   panel->w = w;
   panel->h = h;
   panel->bgColor = bgColor;
   initNineSlice(&panel->border, sprites, 0, 0, w, h, borderSprite, cap, cap);
}

void drawDialogPanel(DialogPanel *panel)
{
   int screenWidth  = getGfxScreenWidth();
   int screenHeight = getGfxScreenHeight();
   panel->x = (screenWidth - panel->w) / 2;
   panel->y = (screenHeight - panel->h) / 2;

   fillGfxRectangle(0, 0, screenWidth, screenHeight, COLOR_SCRIM);
   fillGfxRectangle(panel->x, panel->y, panel->w, panel->h, panel->bgColor);
   moveNineSlice(&panel->border, panel->x, panel->y);
   drawNineSlice(&panel->border);
}
