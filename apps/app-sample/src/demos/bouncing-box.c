// bouncing box - basic movement and collision exercise
#include "bouncing-box.h"
#include "gfx.h"
#include "colors.h"
#include "ui/rectangle.h"

#define BOUNCING_BOX_SIZE 60

static Rectangle box;
static int bbDx = 3, bbDy = 2;

void initBouncingBox(void)
{
   initRectangle(&box, 100, 80, BOUNCING_BOX_SIZE, BOUNCING_BOX_SIZE, COLOR_WHITE);
}

void updateBouncingBox(void)
{
   int sw = getGfxScreenWidth();
   int sh = getGfxScreenHeight();

   box.x += bbDx;
   box.y += bbDy;
   if (box.x <= 0 || box.x + BOUNCING_BOX_SIZE >= sw) bbDx = -bbDx;
   if (box.y <= 0 || box.y + BOUNCING_BOX_SIZE >= sh) bbDy = -bbDy;
}

void drawBouncingBox(void)
{
   drawRectangle(&box);
}
