#pragma once

// stats - on-screen diagnostics overlay (FPS, VRAM usage).
// Hidden by default; press L3 + R3 together to toggle. Call updateStats() once
// per frame (before drawing) to handle the toggle, drawStats() while drawing,
// and termStats() at shutdown to release its font/text VRAM.

#include <stdint.h>

void initStats(int x, int y, int size, uint32_t color);
void updateStats(void);  // handles the L3+R3 visibility toggle
void drawStats(void);
void termStats(void);
