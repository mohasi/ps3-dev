#pragma once

// shared ppu thread helpers for vsh plugins. consolidates priority,
// stack, and create-flag magic numbers used across plugin entry points,
// listener loops, and bridge clients. spawnThread/spawnJoinableThread are
// thin verb-first wrappers around sys_ppu_thread_create; priority is
// always passed in explicitly so scheduling intent reads clearly at the
// call site.

#include <sys/ppu_thread.h>
#include <sys/synchronization.h>
#include <sys/timer.h>

#include "syscall.h"   // scCall1, for the raw thread-exit in exitLoaderThread

// background plugin threads. 0x400 is the conventional vsh-plugin priority
// (matches webMAN/cobra). lower number = higher priority on lv2.
//   HIGH    (0x3E8): audio mixer / anything that must not underrun.
//   DEFAULT (0x400): plugin work, listeners, dispatch loops.
//   LOW     (0x500): background workers that must yield to listeners
//                    (e.g. per-session FTP transfer thread, so accept()
//                    on :21 stays responsive under load).
// stack sizes follow the SYS_PROCESS_PARAM naming style so call sites
// read consistently.
#define THREAD_PRIORITY_HIGH     0x3E8
#define THREAD_PRIORITY_DEFAULT  0x400
#define THREAD_PRIORITY_LOW      0x500
#define THREAD_STACK_SIZE_8KB    0x2000
#define THREAD_STACK_SIZE_16KB   0x4000
#define THREAD_STACK_SIZE_64KB   0x10000

// sys_ppu_thread_create takes 0 for detached; SYS_PPU_THREAD_CREATE_JOINABLE
// is already named, so mirror it for the detached case.
#define THREAD_CREATE_DETACHED   0

// spawn a detached background thread. caller picks priority explicitly
// so scheduling intent is visible at every call site; pass THREAD_PRIORITY_DEFAULT
// for the common case. returns the lv2 rc; tid is filled in on success
// but generally unused since the thread is detached.
static inline int spawnThread(sys_ppu_thread_t *tid, void (*entry)(uint64_t), uint64_t arg, int priority, size_t stack, const char *name)
{
   return sys_ppu_thread_create(tid, entry, arg, priority, stack, THREAD_CREATE_DETACHED, name);
}

// spawn a joinable background thread. use when the caller needs to
// sys_ppu_thread_join() for clean teardown (e.g. prx unload).
static inline int spawnJoinableThread(sys_ppu_thread_t *tid, void (*entry)(uint64_t), uint64_t arg, int priority, size_t stack, const char *name)
{
   return sys_ppu_thread_create(tid, entry, arg, priority, stack, SYS_PPU_THREAD_CREATE_JOINABLE, name);
}

// lightweight mutex helpers wrapping the lv2 lwmutex primitives. lock/unlock
// take the default priority-order attribute. createLock is non-recursive by
// default (the safer choice); createRecursiveLock is for paths that may
// re-enter their own critical section (e.g. log tee inside a held lock).
static inline int createLock(sys_lwmutex_t *m)
{
   sys_lwmutex_attribute_t attr;
   sys_lwmutex_attribute_initialize(attr);
   return sys_lwmutex_create(m, &attr);
}

static inline int createRecursiveLock(sys_lwmutex_t *m)
{
   sys_lwmutex_attribute_t attr;
   sys_lwmutex_attribute_initialize(attr);
   attr.attr_recursive = SYS_SYNC_RECURSIVE;
   return sys_lwmutex_create(m, &attr);
}

static inline int destroyLock(sys_lwmutex_t *m) { return sys_lwmutex_destroy(m); }
static inline int lock(sys_lwmutex_t *m)        { return sys_lwmutex_lock(m, 0); }
static inline int unlock(sys_lwmutex_t *m)      { return sys_lwmutex_unlock(m); }

// readable aliases for the two awkwardly-named lv2 primitives every
// background thread uses. exitThread() ends the current detached worker;
// sleepMs() is the obvious wrapper around sys_timer_usleep's microsecond api.
// yieldThread() lets the scheduler run other ready threads of equal/higher
// priority for one quantum before returning; if nothing else is ready it
// returns immediately. use to give the kernel breathing room between
// syscall-heavy loop iterations without parking the thread.
static inline void exitThread(void)     { sys_ppu_thread_exit(0); }
static inline void sleepMs(unsigned ms) { sys_timer_usleep(ms * 1000); }
static inline void yieldThread(void)    { sys_ppu_thread_yield(); }

// end a module's _start/_stop. the kernel runs the entry on a loader thread and
// joins on it; HEN 4.93 hard-locks the console if that thread returns normally
// (the return routes through the user-mode thread-exit). exit with the raw lv2
// syscall instead. call at the end of _start/_stop in place of the return; the
// module still goes resident because the kernel finalizes it after the join.
// matches every working webMAN/cobra plugin (see webftp main.c, VshFpsCounter).
static inline void exitLoaderThread(void)
{
   scCall1(41, 0);   // lv2 syscall 41 = sys_ppu_thread_exit
}

// join a previously-spawned joinable thread, discarding its exit status.
// callers that need the status can still use sys_ppu_thread_join directly,
// but every existing teardown path here just wants "wait until it's gone".
static inline int joinThread(sys_ppu_thread_t tid)
{
   uint64_t status = 0;
   return sys_ppu_thread_join(tid, &status);
}
