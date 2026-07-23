#pragma once

// run-sample - one point in a run's history, shared by the recorder that writes it
// and the graph that draws it.
//
// temperatures are in tenths of a degree: whole degrees drew a visible staircase.
// elapsed time is full width, because 16 bits wrapped after 18 hours and blanked
// the graph on a long run.

#include <stdint.h>

typedef struct RunSample {
   uint32_t elapsedSeconds;
   uint16_t cpuTenthsC;
   uint16_t rsxTenthsC;
   uint8_t  fanPercent;
} RunSample;

// a run's samples handed over as one value, so nothing has to pass an array and
// its count separately and keep the two in step.
typedef struct RunSamples {
   const RunSample *samples;
   int count;
} RunSamples;
