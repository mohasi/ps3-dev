#include "clocks.h"
#include "thread.h"
#include "dbg.h"

static int bootCoreMhz, bootMemoryMhz;

int getBootRsxCoreClockMhz(void)   { return bootCoreMhz; }
int getBootRsxMemoryClockMhz(void) { return bootMemoryMhz; }

void initClocks(void)
{
   bootCoreMhz   = getRsxCoreClockMhz();
   bootMemoryMhz = getRsxMemoryClockMhz();
   logInfo("[bench] rsx clocks at startup: core %d MHz, memory %d MHz\n", bootCoreMhz, bootMemoryMhz);
}

void restoreBootRsxClocks(void)
{
   if (bootCoreMhz == 0 || bootMemoryMhz == 0) return;   // nothing was readable, so nothing to put back

   if (getRsxCoreClockMhz() != bootCoreMhz && getRsxClockMultiplier(RSX_CORE_CLOCK_REGISTER) != 0)
      setRsxClockMultiplier(RSX_CORE_CLOCK_REGISTER, bootCoreMhz / RSX_CORE_STEP_MHZ);

   // the memory clock must not jump, so walk it back one step per settling period. stepRsxMemoryClock
   // refuses a step that comes too soon, so the wait here is what lets each one through.
   for (int step = 0; step < (RSX_MEMORY_MAX_MHZ - RSX_MEMORY_MIN_MHZ) / RSX_MEMORY_STEP_MHZ; step++)
   {
      int nowMhz = getRsxMemoryClockMhz();
      if (nowMhz == 0) return;             // unreadable: cfw peek/poke unavailable
      if (nowMhz == bootMemoryMhz) break;

      stepRsxMemoryClock(nowMhz > bootMemoryMhz ? -1 : +1);
      sleepMs(RSX_MEMORY_STEP_INTERVAL_US / 1000);
   }

   logInfo("[bench] rsx clocks back to boot: core %d MHz, mem %d MHz\n", getRsxCoreClockMhz(), getRsxMemoryClockMhz());
}
