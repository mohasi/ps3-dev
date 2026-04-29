// bouncing box - basic movement and collision exercise
#include "bouncing-box.h"
#include "gfx.h"
#include "colors.h"

#define BOUNCING_BOX_SIZE 60

static int bbX = 100, bbY = 80;
static int bbDx = 3, bbDy = 2;

void moveBouncingBox(void)
{
    int sw = gfxScreenWidth();
    int sh = gfxScreenHeight();

    bbX += bbDx;
    bbY += bbDy;
    if (bbX <= 0 || bbX + BOUNCING_BOX_SIZE >= sw) bbDx = -bbDx;
    if (bbY <= 0 || bbY + BOUNCING_BOX_SIZE >= sh) bbDy = -bbDy;
}

void drawBouncingBox(void)
{
    gfxFillRectangle(bbX, bbY, BOUNCING_BOX_SIZE, BOUNCING_BOX_SIZE, COLOR_WHITE);
}
