#pragma once

// properties-overlay - modal panel of details for one file: name, type, size, modified time,
// permissions and location, plus its SHA-1. The dialog opens straight away and the hash is
// computed on a background worker, so a big file shows "Calculating..." with a percentage while
// the rest of the details are already readable. Circle or cross closes it.

#include "overlay.h"
#include "audio.h"

void initPropertiesOverlay(Audio *clickSfx);

// opens the dialog for one file (absolute path). folders are not supported - the caller gates.
void showProperties(const char *path);

void rethemePropertiesOverlay(void);   // recolour the text for a live theme switch

extern Overlay propertiesOverlay;
