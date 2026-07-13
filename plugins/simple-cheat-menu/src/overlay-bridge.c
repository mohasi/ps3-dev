// C<->C++ bridge for overlay.cpp: it can't include the C-only dbg.h / syscall.h,
// so these thin shims lend it logging and the lv2 heap. kept here (out of prx.c
// and the trigger state machine) so each file has one job.

#include <stdint.h>

#include "dbg.h"
#include "syscall.h"

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
