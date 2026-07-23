#include <sys/sys_time.h>

#include "clocks.h"
#include "syscall.h"
#include "thread.h"
#include "dbg.h"

// hypervisor registers holding the two clock multipliers
#define CORE_CLOCK_REGISTER   0x28000004028ULL
#define MEMORY_CLOCK_REGISTER 0x28000004010ULL

// the multiplier sits in the third byte of the register's second word
#define MULTIPLIER_SHIFT 8
#define MULTIPLIER_MASK  0xFFULL

#define CORE_STEP_MHZ   50
#define MEMORY_STEP_MHZ 25

// stay well inside what the hardware is known to tolerate. underclocking is as
// useful as overclocking here - a cooler stable console is a valid answer.
#define CORE_MIN_MHZ   300
#define CORE_MAX_MHZ   800
#define MEMORY_MIN_MHZ 400
#define MEMORY_MAX_MHZ 900

// the memory clock hangs the console if it is moved faster than one 25 MHz step
// per settling period - both cobra and webman ramp it with a wait between pokes.
// button mashing must not get past that, so steps are rate limited here.
#define MEMORY_STEP_INTERVAL_US 125000

static int bootCoreMhz, bootMemoryMhz;

static int getMultiplier(uint64_t registerAddress)
{
   return (int)((peekLv1(registerAddress) >> MULTIPLIER_SHIFT) & MULTIPLIER_MASK);
}

static void setMultiplier(uint64_t registerAddress, int multiplier)
{
   uint64_t value = peekLv1(registerAddress);
   value = (value & ~(MULTIPLIER_MASK << MULTIPLIER_SHIFT)) | ((uint64_t)multiplier << MULTIPLIER_SHIFT);
   pokeLv1(registerAddress, value);
}

// the live register is the only source: a console that cannot be read cannot be
// changed either, and reporting a clock we could not change would be a lie.
int getRsxCoreClockMhz(void)   { return getMultiplier(CORE_CLOCK_REGISTER) * CORE_STEP_MHZ; }
int getRsxMemoryClockMhz(void) { return getMultiplier(MEMORY_CLOCK_REGISTER) * MEMORY_STEP_MHZ; }

int getBootRsxCoreClockMhz(void)   { return bootCoreMhz; }
int getBootRsxMemoryClockMhz(void) { return bootMemoryMhz; }

void initClocks(void)
{
   bootCoreMhz   = getRsxCoreClockMhz();
   bootMemoryMhz = getRsxMemoryClockMhz();
   logInfo("[bench] rsx clocks at startup: core %d MHz, memory %d MHz\n", bootCoreMhz, bootMemoryMhz);
}

static void stepClock(uint64_t registerAddress, int stepMhz, int direction, int minMhz, int maxMhz, const char *name)
{
   int multiplier = getMultiplier(registerAddress);
   if (multiplier == 0) { logWarn("[bench] %s clock is unreadable; cfw peek/poke unavailable\n", name); return; }

   int wanted = (multiplier + direction) * stepMhz;
   if (wanted < minMhz || wanted > maxMhz)
   {
      logInfo("[bench] %s clock stays at %d MHz (%d is outside %d-%d)\n",
              name, multiplier * stepMhz, wanted, minMhz, maxMhz);
      return;
   }

   setMultiplier(registerAddress, multiplier + direction);
   logInfo("[bench] %s clock now %d MHz (asked for %d)\n", name, getMultiplier(registerAddress) * stepMhz, wanted);
}

void stepRsxCoreClock(int direction)
{
   stepClock(CORE_CLOCK_REGISTER, CORE_STEP_MHZ, direction, CORE_MIN_MHZ, CORE_MAX_MHZ, "core");
}

void stepRsxMemoryClock(int direction)
{
   static uint64_t lastStepUs;
   uint64_t now = sys_time_get_system_time();
   if (now - lastStepUs < MEMORY_STEP_INTERVAL_US) return;
   lastStepUs = now;

   stepClock(MEMORY_CLOCK_REGISTER, MEMORY_STEP_MHZ, direction, MEMORY_MIN_MHZ, MEMORY_MAX_MHZ, "memory");
}

void restoreBootRsxClocks(void)
{
   if (bootCoreMhz == 0 || bootMemoryMhz == 0) return;   // nothing was readable, so nothing to put back

   if (getMultiplier(CORE_CLOCK_REGISTER) != 0 && getRsxCoreClockMhz() != bootCoreMhz)
      setMultiplier(CORE_CLOCK_REGISTER, bootCoreMhz / CORE_STEP_MHZ);

   // the memory clock must not jump, so walk it back one step per settling period
   for (int step = 0; step < (MEMORY_MAX_MHZ - MEMORY_MIN_MHZ) / MEMORY_STEP_MHZ; step++)
   {
      int multiplier = getMultiplier(MEMORY_CLOCK_REGISTER);
      if (multiplier == 0) return;
      int nowMhz = multiplier * MEMORY_STEP_MHZ;
      if (nowMhz == bootMemoryMhz) break;

      setMultiplier(MEMORY_CLOCK_REGISTER, multiplier + (nowMhz > bootMemoryMhz ? -1 : 1));
      sleepMs(MEMORY_STEP_INTERVAL_US / 1000);
   }

   logInfo("[bench] rsx clocks back to boot: core %d MHz, mem %d MHz\n", getRsxCoreClockMhz(), getRsxMemoryClockMhz());
}
