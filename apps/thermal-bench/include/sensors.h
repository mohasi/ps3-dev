#pragma once

// sensors - one reading of everything the console will tell us about its
// cooling state: temperatures, fan duty and the rsx clocks.

#include "syscall.h"

typedef struct Sensors {
   int cpuTenthsC, rsxTenthsC;     // cell and gpu temperature, in tenths of a degree
   int temperatureReadable;        // 0 when either temperature syscall refused us
   int fanPercent;                 // 0-100, the duty the fan is driven at
   FanMode fanMode;
   int fanReadable;                // 0 when the fan syscall refused us
   int coreClockMhz, memoryClockMhz;
} Sensors;

void readSensors(Sensors *out);

// logs one line whenever a reading moves, so the log holds the history without
// a line per second of nothing happening.
void logSensorChanges(const Sensors *sensors);

// "automatic" / "manual" / "full" - what is deciding the fan duty right now.
const char *getFanModeText(FanMode mode);
