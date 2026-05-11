#pragma once

// free-space-widget - hdd free space label refreshed periodically

#include "font.h"

void initFreeSpaceWidget(Font *font, int x, int y, int size, uint32_t color, int width);
void updateFreeSpaceWidget(void);
void drawFreeSpaceWidget(void);
