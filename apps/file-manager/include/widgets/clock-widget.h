#pragma once

// clock-widget - date/time label refreshed every second

#include "font.h"

void initClockWidget(Font *font, int x, int y, int size);
void rethemeClockWidget(void);   // reapply the active theme for a live switch
void updateClockWidget(void);
void drawClockWidget(void);
void termClockWidget(void);
