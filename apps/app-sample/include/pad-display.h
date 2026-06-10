#pragma once

// pad display - debug visualization of controller state

#include "font.h"
#include <stdint.h>

void initPadDisplay(Font *f, int x, int y, int size, uint32_t color);
void drawPadDisplay(void);
void termPadDisplay(void);
