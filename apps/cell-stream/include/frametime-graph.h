#pragma once

// frametime-graph - draws a per-frame frame-time (ms) bar graph from a FrameTiming. app-side
// rendering only; the timing itself lives in simple-lib-core's frame-timing.h so a vsh plugin
// can share the maths.

#include "frame-timing.h"

int  getFrametimeGraphHeight(void);   // pixel height it draws, for panel layout
void drawFrametimeGraph(const FrameTiming *timing, int x, int y, int width);
