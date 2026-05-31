#pragma once

#include "overlay.h"

void beginProgress(const char *title);
void setProgress(int done, int total, const char *currentItem);
void endProgress(void);

extern Overlay progressOverlay;
