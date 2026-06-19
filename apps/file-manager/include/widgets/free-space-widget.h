#pragma once

// free-space-widget - free space label for the current volume, refreshed periodically

#include "font.h"

void initFreeSpaceWidget(Font *font, int x, int y, int size, uint32_t color, int width);
void updateFreeSpaceWidget(void);

// report free space for the volume that owns path (the directory in view); the
// VFS routes it to the right backend. call when the current directory changes.
void setFreeSpacePath(const char *path);
void drawFreeSpaceWidget(void);
void termFreeSpaceWidget(void);
