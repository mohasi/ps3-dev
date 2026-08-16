// C<->C++ bridge for overlay.cpp: it can't include the C-only dbg.h / syscall.h,
// so these thin shims lend it logging and the lv2 heap. kept here (out of prx.c
// and the trigger state machine) so each file has one job.

#include <stdint.h>

#include "dbg.h"
#include "syscall.h"
#include "rsx-clocks.h"   // getRsxCoreClockMhz / getRsxMemoryClockMhz for the stats counter

#define TAG "[cht] "

// step-logging: each call is one disk-flushed line, so on a hard lock the last
// line written names the exact step reached before the locking instruction.
void overlayLog(const char *msg) { logInfo(TAG "%s\n", msg); }
void overlayLogHex(const char *msg, unsigned int value) { logInfo(TAG "%s=0x%x\n", msg, value); }

// heap-on-demand: one page holding the overlay's per-session storage, taken on the
// first menu open. sys_memory_allocate with SYS_PAGE_64K needs a 64K-aligned size
// (it rejects, not rounds, an unaligned one) — round up to the next 64K page.
void *overlayHeapAlloc(unsigned int size)
{
   uint32_t addr = 0;
   unsigned int pageSize = (size + 0xFFFFu) & ~0xFFFFu;
   return sysMemAllocate(pageSize, SYS_PAGE_64K, &addr) == 0 ? (void *)(uintptr_t)addr : 0;
}
void overlayHeapFree(void *ptr)
{
   if (ptr) sysMemFree((uint32_t)(uintptr_t)ptr);
}

// Clocks and temperatures for the stats counter. These are syscalls and a hypervisor peek, so
// they are read on a worker thread a couple of times a second and never on the drawing thread.
// Temperatures come back in tenths of a degree; a clock reads 0 when cfw peek is unavailable.
void readStatsSensors(int *coreMhz, int *memoryMhz, int *cpuTenths, int *rsxTenths)
{
   *coreMhz   = getRsxCoreClockMhz();
   *memoryMhz = getRsxMemoryClockMhz();

   uint32_t raw = 0;
   *cpuTenths = getConsoleTemperature(THERMAL_ZONE_CPU, &raw) == 0
              ? getTemperatureCelsius(raw) * 10 + getTemperatureTenths(raw) : 0;
   raw = 0;
   *rsxTenths = getConsoleTemperature(THERMAL_ZONE_RSX, &raw) == 0
              ? getTemperatureCelsius(raw) * 10 + getTemperatureTenths(raw) : 0;
}

// Overclocking, driven from the menu's Stats Counter tab. Both go through the shared header, so
// the memory clock's settling period is enforced there and cannot be bypassed by holding the
// button down. Logged because a clock change is worth being able to trace after the fact.
void stepStatsCoreClock(int direction)
{
   int mhz = stepRsxCoreClock(direction);
   if (mhz) logInfo(TAG "rsx core clock now %d MHz\n", mhz);
   else     logWarn(TAG "rsx core clock step refused (limit reached or peek unavailable)\n");
}

void stepStatsMemoryClock(int direction)
{
   int mhz = stepRsxMemoryClock(direction);
   if (mhz) logInfo(TAG "rsx memory clock now %d MHz\n", mhz);
}
