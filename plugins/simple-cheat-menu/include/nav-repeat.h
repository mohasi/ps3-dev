#pragma once

#include <stdint.h>

// d-pad auto-repeat for a held direction, driven by fixed-interval poll ticks (the
// cheat menu polls at ~33ms). navFire returns 1 the tick the direction should step:
// immediately on the press edge, then after NAV_INITIAL_TICKS while held, repeating
// every NAV_REPEAT_TICKS, and every NAV_FAST_TICKS once held past NAV_FAST_AFTER
// repeats — so paging a long list speeds up instead of crawling. one NavRepeat per
// axis (up, down); release resets it. mirrors simple-lib-app's button-repeat feel
// (that one is time-based and app-only; this is tick-based for the vsh plugin).
#define NAV_INITIAL_TICKS   9   // ~300ms before the first auto-repeat
#define NAV_REPEAT_TICKS    2   // ~66ms between repeats
#define NAV_FAST_AFTER     10   // repeats at the base rate before speeding up
#define NAV_FAST_TICKS      1   // every poll (~33ms) once accelerated

typedef struct { uint32_t ticksHeld; int repeats; int wasDown; } NavRepeat;

static inline int navFire(NavRepeat *nav, int down)
{
   if (!down) { nav->ticksHeld = 0; nav->repeats = 0; nav->wasDown = 0; return 0; }
   if (!nav->wasDown) { nav->wasDown = 1; nav->ticksHeld = 0; nav->repeats = 0; return 1; }   // press edge fires
   nav->ticksHeld++;
   uint32_t threshold = nav->repeats == 0 ? NAV_INITIAL_TICKS
                      : nav->repeats >= NAV_FAST_AFTER ? NAV_FAST_TICKS : NAV_REPEAT_TICKS;
   if (nav->ticksHeld >= threshold) { nav->ticksHeld = 0; nav->repeats++; return 1; }
   return 0;
}
