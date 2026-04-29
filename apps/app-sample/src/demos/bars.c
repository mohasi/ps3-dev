// colored bars - rectangle primitives exercise
#include "bars.h"
#include "gfx.h"
#include "colors.h"

void drawBars(void)
{
    gfxFillRectangle(40, 40, 200, 30, COLOR_ROSE_500);
    gfxFillRectangle(40, 80, 200, 30, COLOR_INDIGO_800);
    gfxFillRectangle(40, 120, 200, 30, COLOR_EMERALD_400);
}
