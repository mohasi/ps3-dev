#pragma once

// shared ppu thread defaults for vsh plugins. consolidates priority,
// stack, and create-flag magic numbers used across plugin entry points,
// listener loops, and bridge clients. spawnThread() is a thin verb-first
// wrapper around sys_ppu_thread_create that picks sensible defaults.

#include <sys/ppu_thread.h>

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
