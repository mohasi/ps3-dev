#pragma once

// clock-widget - date/time label refreshed every second

#include "font.h"

void initClockWidget(Font *font, int x, int y, int size, uint32_t color);
void updateClockWidget(void);
void drawClockWidget(void);
void termClockWidget(void);
