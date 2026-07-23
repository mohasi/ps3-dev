#pragma once

// console-model - which ps3 this is, read from the console id the factory wrote
// into flash.
//
// why the app cares: every console up to the CECHH generation carries the 90 nm
// graphics chip, whose solder underfill weakens from around 70 C - well below
// what the later chips put up with. those consoles get a lower default safety
// cutoff, so a stress run cannot cook the part it is measuring.
//
// the console id is written at the factory and never changes, so a console whose
// graphics chip was swapped for a later one still reads as its original model.
// the settings file is the way round that.

// the two cutoffs a model can carry. the fragile one is also what an unidentified
// console gets, and the modern one is the ceiling for a settings.txt override -
// nothing on this console tolerates more.
#define FRAGILE_CUTOFF_CELSIUS 70
#define MODERN_CUTOFF_CELSIUS  88

// the model as one line for the screen and the run file: "CECHH / M / Q (DIA-001),
// 90 nm graphics chip", or "unknown console model". read once, on the first call.
const char *getConsoleModelSummary(void);

int getModelSafetyCutoffCelsius(void);
