// frametime-graph - see frametime-graph.h. one frame-time (ms) bar graph: a colored bar per frame
// (green under ~20ms, yellow under ~33ms, red above), with reference lines at 60fps (16.7ms) and
// 30fps (33.3ms). the timing itself lives in simple-lib-core's frame-timing.h.

#include "frametime-graph.h"
#include "gfx.h"

#define FT_FULL_SCALE_US 50000   // graph tops out at 50ms
#define FT_HEIGHT        56

#define BACKGROUND   0x80000000
#define REF60_COLOR  0xFF2E8B57   // 16.7ms / 60fps reference line
#define REF30_COLOR  0xFF8B7500   // 33.3ms / 30fps reference line
#define GOOD_US      20000
#define WARN_US      33334
#define GOOD_COLOR   0xFF3DD56D
#define WARN_COLOR   0xFFE0C000
#define BAD_COLOR    0xFFE04040

int getFrametimeGraphHeight(void) { return FT_HEIGHT; }

static uint32_t barColor(uint32_t us)
{
   if (us <= GOOD_US) return GOOD_COLOR;
   if (us <= WARN_US) return WARN_COLOR;
   return BAD_COLOR;
}

void drawFrametimeGraph(const FrameTiming *timing, int x, int y, int width)
{
   uint32_t history[FRAME_TIMING_HISTORY];
   int columns = getFrameHistory(timing, history, FRAME_TIMING_HISTORY);

   fillGfxRectangle(x, y, width, FT_HEIGHT, BACKGROUND);
   fillGfxRectangle(x, y + FT_HEIGHT - 16667 * FT_HEIGHT / FT_FULL_SCALE_US, width, 1, REF60_COLOR);
   fillGfxRectangle(x, y + FT_HEIGHT - 33334 * FT_HEIGHT / FT_FULL_SCALE_US, width, 1, REF30_COLOR);

   // stretch the samples across the full width so the graph is never left-empty: sample i owns the
   // x-slice [i*width/columns, (i+1)*width/columns).
   for (int i = 0; i < columns; i++) {
      uint32_t us = history[i] > FT_FULL_SCALE_US ? FT_FULL_SCALE_US : history[i];
      int barHeight = (int)(us * FT_HEIGHT / FT_FULL_SCALE_US);
      int barLeft = x + i * width / columns;
      int barWidth = x + (i + 1) * width / columns - barLeft;
      if (barWidth < 1) barWidth = 1;
      fillGfxRectangle(barLeft, y + FT_HEIGHT - barHeight, barWidth, barHeight, barColor(history[i]));
   }
}
