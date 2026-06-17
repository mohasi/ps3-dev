#pragma once

// button-repeat - turns "press" + "hold" into a repeating fire signal.
//
// each axis the caller cares about (e.g. an up/down scroll axis) gets one
// ButtonRepeat. call isRepeatDue() with the current pad state for each button
// on that axis; it returns true the frame the button should fire. on initial
// press it fires immediately, then while held it fires once after
// BUTTON_HOLD_INITIAL_US and then every BUTTON_HOLD_REPEAT_US.

#include "pad.h"
#include <stdint.h>
#include <sys/sys_time.h>

#define BUTTON_HOLD_INITIAL_US 300000  // delay before the first auto-repeat
#define BUTTON_HOLD_REPEAT_US   60000  // interval between subsequent auto-repeats

typedef struct {
   uint64_t timer;
   int      repeats;
} ButtonRepeat;

static inline int isRepeatDue(ButtonRepeat *r, PadButtonState state)
{
   if (state == PAD_BUTTON_STATE_PRESSED) {
      r->timer   = sys_time_get_system_time();
      r->repeats = 0;
      return 1;
   }
   if (state == PAD_BUTTON_STATE_HELD) {
      uint64_t now       = sys_time_get_system_time();
      uint64_t threshold = r->repeats == 0 ? BUTTON_HOLD_INITIAL_US : BUTTON_HOLD_REPEAT_US;
      if (now - r->timer >= threshold) {
         r->timer = now;
         r->repeats++;
         return 1;
      }
   }
   return 0;
}
