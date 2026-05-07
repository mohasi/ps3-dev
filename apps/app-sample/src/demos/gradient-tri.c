// gradient triangle - GPU-rendered with per-vertex color interpolation
#include "gradient-tri.h"
#include "gfx.h"
#include "colors.h"

void drawGradientTriangle(void)
{
    gfxDrawTriangle(
        1248.0f, 864.0f, COLOR_RED,
        1824.0f, 864.0f, COLOR_GREEN,
        1536.0f, 432.0f, COLOR_BLUE
    );
}
