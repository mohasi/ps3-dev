// gradient triangle - GPU-rendered with per-vertex color interpolation
#include "gradient-tri.h"
#include "gfx.h"
#include "colors.h"

void drawGradientTriangle(void)
{
    gfxDrawTriangle(
         0.3f, -0.6f, COLOR_RED,
         0.9f, -0.6f, COLOR_GREEN,
         0.6f,  0.2f, COLOR_BLUE
    );
}
