#pragma once

// shared ppu thread defaults for vsh plugins. consolidates priority,
// stack, and create-flag magic numbers used across plugin entry points,
// listener loops, and bridge clients. spawnThread() is a thin verb-first
// wrapper around sys_ppu_thread_create that picks sensible defaults.

#include <sys/ppu_thread.h>
#include <sys/synchronization.h>
#include <sys/timer.h>

// background plugin threads. 0x400 is the conventional vsh-plugin priority
// (matches webMAN/cobra). stack sizes follow the SYS_PROCESS_PARAM naming
// style (PROCESS_STACK_SIZE_64KB etc.) so call sites read consistently.
#define THREAD_PRIORITY_DEFAULT  0x400
#define THREAD_STACK_SIZE_8KB    0x2000
#define THREAD_STACK_SIZE_16KB   0x4000

// sys_ppu_thread_create takes 0 for detached; SYS_PPU_THREAD_CREATE_JOINABLE
// is already named, so mirror it for the detached case.
#define THREAD_CREATE_DETACHED   0

// spawn a detached background thread with the standard defaults. returns
// the lv2 rc; tid is filled in on success but generally unused since the
// thread is detached.
static inline int spawnThread(sys_ppu_thread_t *tid, void (*entry)(uint64_t), uint64_t arg, size_t stack, const char *name)
{
    return sys_ppu_thread_create(tid, entry, arg, THREAD_PRIORITY_DEFAULT, stack, THREAD_CREATE_DETACHED, name);
}

// spawn a joinable background thread. use when the caller needs to
// sys_ppu_thread_join() for clean teardown (e.g. prx unload).
static inline int spawnJoinableThread(sys_ppu_thread_t *tid, void (*entry)(uint64_t), uint64_t arg, size_t stack, const char *name)
{
    return sys_ppu_thread_create(tid, entry, arg, THREAD_PRIORITY_DEFAULT, stack, SYS_PPU_THREAD_CREATE_JOINABLE, name);
}

// lightweight recursive mutex helpers. all three wrap the lv2 lwmutex
// primitives with vsh-friendly defaults (recursive + priority order),
// matching how plugins already use them ad-hoc. returns the lv2 rc.
static inline int createLock(sys_lwmutex_t *m)
{
    sys_lwmutex_attribute_t attr;
    sys_lwmutex_attribute_initialize(attr);
    attr.attr_recursive = SYS_SYNC_RECURSIVE;
    return sys_lwmutex_create(m, &attr);
}

static inline int lock(sys_lwmutex_t *m)   { return sys_lwmutex_lock(m, 0); }
static inline int unlock(sys_lwmutex_t *m) { return sys_lwmutex_unlock(m); }

// readable aliases for the two awkwardly-named lv2 primitives every
// background thread uses. exitThread() ends the current detached worker;
// sleepMs() is the obvious wrapper around sys_timer_usleep's microsecond api.
static inline void exitThread(void)     { sys_ppu_thread_exit(0); }
static inline void sleepMs(unsigned ms) { sys_timer_usleep(ms * 1000); }
