#pragma once

// toast - a short message that appears top-right for a couple of seconds, then fades
// out on its own. used to name the new state whenever a shortcut switches a mode.
// call updateToast()/drawToast() once per frame regardless of state - both no-op while idle.

#include "font.h"

void initToast(Font *font);
void showToast(const char *message);
void updateToast(void);
void drawToast(void);
void freeToast(void);
