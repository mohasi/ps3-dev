#pragma once

// safety - the rule that outranks everything else in this app: the tool must never
// be the thing that cooks the console.
//
// watches every sensor reading while the console is being pushed, and past the
// cutoff - or when the console will not report a temperature at all - drops the
// load to off and puts the graphics clocks back to what they booted at. raising a
// load dial again re-arms it.

#include "sensors.h"

void updateSafety(const Sensors *sensors);

int hasSafetyCutoffFired(void);
