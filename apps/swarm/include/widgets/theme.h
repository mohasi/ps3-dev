#pragma once

// the few colours the whole app is drawn from, so the frame and the rows stay in step

#include "colors.h"

#define BACKDROP      COLOR_BLACK
#define PANEL_FILL    0xFF121212   // the panels sit a shade above the backdrop, as in the mockup
#define PANEL_BORDER  COLOR_NEUTRAL_800
#define ROW_SEPARATOR COLOR_NEUTRAL_800

#define PICKED_FILL   0xFF0A1929   // the blue-black behind the picked row in the mockup
#define PICKED_BORDER COLOR_SKY_500
#define RESTING_BORDER COLOR_NEUTRAL_700   // the picked item while the pad is driving the other half

#define TEXT_BRIGHT COLOR_WHITE
#define TEXT_PLAIN  COLOR_NEUTRAL_300
#define TEXT_QUIET  COLOR_NEUTRAL_400
#define TEXT_FAINT  COLOR_NEUTRAL_500
