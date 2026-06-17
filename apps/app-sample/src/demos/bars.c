// colored bars - rectangle primitives exercise
#include "bars.h"
#include "gfx.h"
#include "colors.h"
#include "ui/rectangle.h"

static Rectangle bars[3];

void initBars(void)
{
   initRectangle(&bars[0], 40, 40, 200, 30, COLOR_ROSE_500);
   initRectangle(&bars[1], 40, 80, 200, 30, COLOR_INDIGO_800);
   initRectangle(&bars[2], 40, 120, 200, 30, COLOR_EMERALD_400);
}

void drawBars(void)
{
   for (int i = 0; i < 3; i++)
      drawRectangle(&bars[i]);
}
