#pragma once

// RSX core and memory clock speeds, read from the hypervisor registers that hold their
// multipliers. Measured on Mohammed's console and matching webMAN and the VshFpsCounter
// reference, which read the same two addresses.
//
// Reading is safe on any cfw with peek enabled. Writing is how the RSX is overclocked, and the
// memory clock hangs the console if it is moved faster than one 25 MHz step per settling period,
// which is why stepRsxMemoryClock rate limits itself. Both cobra and webMAN ramp it the same way.

#include "syscall.h"   // peekLv1 / pokeLv1
#include <sys/sys_time.h>   // sys_time_get_system_time, for the memory clock's settling period

#define RSX_CORE_CLOCK_REGISTER    0x28000004028ULL
#define RSX_MEMORY_CLOCK_REGISTER  0x28000004010ULL

// the multiplier sits in the third byte of the register's second word
#define RSX_MULTIPLIER_SHIFT  8
#define RSX_MULTIPLIER_MASK   0xFFULL

#define RSX_CORE_STEP_MHZ    50
#define RSX_MEMORY_STEP_MHZ  25

static inline int getRsxClockMultiplier(uint64_t registerAddress)
{
   return (int)((peekLv1(registerAddress) >> RSX_MULTIPLIER_SHIFT) & RSX_MULTIPLIER_MASK);
}

// 0 when the register cannot be read, which is what a console with cfw syscalls disabled reports
static inline int getRsxCoreClockMhz(void)
{
   return getRsxClockMultiplier(RSX_CORE_CLOCK_REGISTER) * RSX_CORE_STEP_MHZ;
}

static inline int getRsxMemoryClockMhz(void)
{
   return getRsxClockMultiplier(RSX_MEMORY_CLOCK_REGISTER) * RSX_MEMORY_STEP_MHZ;
}

// Stay well inside what the hardware is known to tolerate. Underclocking is as useful as
// overclocking here: a cooler stable console is a valid answer.
#define RSX_CORE_MIN_MHZ    300
#define RSX_CORE_MAX_MHZ    800
#define RSX_MEMORY_MIN_MHZ  400
#define RSX_MEMORY_MAX_MHZ  900

// the memory clock hangs the console if it is moved faster than one step per settling period, so
// button mashing must not get past this
#define RSX_MEMORY_STEP_INTERVAL_US  125000

static inline void setRsxClockMultiplier(uint64_t registerAddress, int multiplier)
{
   uint64_t value = peekLv1(registerAddress);
   value = (value & ~(RSX_MULTIPLIER_MASK << RSX_MULTIPLIER_SHIFT)) | ((uint64_t)multiplier << RSX_MULTIPLIER_SHIFT);
   pokeLv1(registerAddress, value);
}

// Move a clock one step (direction +1 / -1), clamped to the safe range. Returns the resulting
// speed in MHz, or 0 if the step was refused: register unreadable, range reached, or — for the
// memory clock — asked for again before its settling period was up. Defined in rsx-clocks.c so
// that rate limit is one timestamp shared by every caller in the process.
int stepRsxCoreClock(int direction);
int stepRsxMemoryClock(int direction);
