#pragma once

// screen-manager - owns the active screen and the screen stack, and drives the
// active screen each frame. screens themselves only implement the Screen
// contract (screen.h); this is the single place that navigates between them.
// the current screen + stack are private to screen-manager.c - reach them only
// through this API.

#include "screen.h"

// replaces the whole stack with newScreen (terminating the current one); pass
// NULL to tear everything down.
void changeScreen(Screen *newScreen);

// suspends the current screen and pushes newScreen on top.
void pushScreen(Screen *newScreen);

// terminates the top screen and resumes the one beneath it.
void popScreen(void);

// drive the active screen for the frame; both no-op when no screen is set.
void updateScreen(void);
void drawScreen(void);
