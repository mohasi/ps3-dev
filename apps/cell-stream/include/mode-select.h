#pragma once

// mode-select - the first screen: which service the pad and the TV are pointed at. Owns its own
// loop and its own labels, so nothing has to be initialised or freed around it.

#include "font.h"

typedef enum {
   STREAM_SOURCE_NONE = -1,   // the user backed out with START
   STREAM_SOURCE_PC,
   STREAM_SOURCE_XBOX_CLOUD,
   STREAM_SOURCE_COUNT
} StreamSource;

StreamSource runModeSelectScreen(Font *font);
