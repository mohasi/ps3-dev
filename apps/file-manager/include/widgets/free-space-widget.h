#pragma once

// free-space-widget - free space label for the current volume, refreshed periodically

#include "font.h"

void initFreeSpaceWidget(Font *font, int x, int y, int size, int width);
void rethemeFreeSpaceWidget(void);   // reapply the active theme for a live switch
void updateFreeSpaceWidget(void);

// report free space for the volume that owns path (the directory in view); the
// VFS routes it to the right backend. call when the current directory changes.
void setFreeSpacePath(const char *path);
void drawFreeSpaceWidget(void);
void termFreeSpaceWidget(void);
