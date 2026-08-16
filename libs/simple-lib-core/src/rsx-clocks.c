#include "rsx-clocks.h"

// Move a clock one step, clamped to the safe range. Returns the resulting speed in MHz, or 0 if
// the register could not be read or the step would leave that range.
static int stepRsxClock(uint64_t registerAddress, int direction, int stepMhz, int minMhz, int maxMhz)
{
   int multiplier = getRsxClockMultiplier(registerAddress);
   if (multiplier == 0) return 0;   // unreadable: cfw peek/poke unavailable

   int wanted = (multiplier + direction) * stepMhz;
   if (wanted < minMhz || wanted > maxMhz) return 0;

   setRsxClockMultiplier(registerAddress, multiplier + direction);
   return getRsxClockMultiplier(registerAddress) * stepMhz;
}

int stepRsxCoreClock(int direction)
{
   return stepRsxClock(RSX_CORE_CLOCK_REGISTER, direction, RSX_CORE_STEP_MHZ, RSX_CORE_MIN_MHZ, RSX_CORE_MAX_MHZ);
}

// The memory clock hangs the console if it is moved faster than one step per settling period, so
// a step that comes too soon is refused. This lives in a .c file, not the header, precisely so
// there is ONE timestamp: as a static inside an inline function every file that called it would
// get its own, and two callers could then step the clock twice within one period.
int stepRsxMemoryClock(int direction)
{
   static uint64_t lastStepUs;

   uint64_t now = sys_time_get_system_time();
   if (now - lastStepUs < RSX_MEMORY_STEP_INTERVAL_US) return 0;
   lastStepUs = now;

   return stepRsxClock(RSX_MEMORY_CLOCK_REGISTER, direction, RSX_MEMORY_STEP_MHZ, RSX_MEMORY_MIN_MHZ, RSX_MEMORY_MAX_MHZ);
}
