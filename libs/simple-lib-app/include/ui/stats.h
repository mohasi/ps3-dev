#pragma once

// stats - fps and vram usage display

#include <stdint.h>

void initStats(int x, int y, int size, uint32_t color);
void drawStats(void);
