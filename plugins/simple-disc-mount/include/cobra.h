#pragma once

// Cobra syscall8 helpers used by the disc mount flow in this plugin.

#include <stdint.h>
#include <sys/timer.h>
#include "dbg.h"
#include "syscall.h"

#define SC_COBRA                8
#define OP_FAKE_STORAGE_EVENT   0x7022
#define DEV_BDVD                0x101000000000006ULL

#define STORAGE_EVENT_PRE_EJECT     4
#define STORAGE_EVENT_EJECTED       8
#define STORAGE_EVENT_PRE_INSERT    7
#define STORAGE_EVENT_INSERTED      3

// Issue syscall8 opcode 0x7024 to switch Cobra's active emulated PS3 disc.
// `files` points to a syscall-visible argv array of ISO part paths.
static inline int cobraMountPs3(char *files[], unsigned int count)
{
    int ret;
    __asm__ volatile (
        "li 3, 0x7024\n\t"
        "mr 4, %[count]\n\t"
        "mr 5, %[files]\n\t"
        "li 11, 8\n\t"
        "sc\n\t"
        "mr %[ret], 3\n\t"
        : [ret]"=r"(ret)
        : [count]"r"((uint64_t)count),
          [files]"r"((uint64_t)(uintptr_t)files)
        : "r0","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12",
          "cr0","ctr","xer","memory"
    );
    if (ret != 0) logError("[sdm] sc8 mount_ps3 rc=0x%x\n", ret);
    return ret;
}

// Raise a synthetic BD-drive storage event so XMB updates disc state.
// The second syscall argument is always 0 for this flow.
static inline int cobraFakeEvent(uint64_t event)
{
    sys_timer_usleep(5000);
    return (int)scCall4(SC_COBRA, OP_FAKE_STORAGE_EVENT, event, 0, DEV_BDVD);
}

static inline int cobraFakeDiscEject(void)
{
    int rc = cobraFakeEvent(STORAGE_EVENT_PRE_EJECT);
    if (rc != 0) {
        logError("[sdm] pre-eject rc=0x%x\n", rc);
        return rc;
    }

    rc = cobraFakeEvent(STORAGE_EVENT_EJECTED);
    if (rc != 0) logError("[sdm] eject rc=0x%x\n", rc);
    return rc;
}

static inline int cobraFakeDiscInsert(void)
{
    int rc = cobraFakeEvent(STORAGE_EVENT_PRE_INSERT);
    if (rc != 0) {
        logError("[sdm] pre-insert rc=0x%x\n", rc);
        return rc;
    }

    rc = cobraFakeEvent(STORAGE_EVENT_INSERTED);
    if (rc != 0) logError("[sdm] insert rc=0x%x\n", rc);
    return rc;
}

// End-to-end mount flow: force XMB eject state, mount ISO via Cobra, then
// publish insert state so the icon appears in the BD slot.
static inline int cobraMountIso(const char *fullPath)
{
    char *files[1];
    files[0] = (char *)fullPath;

    int rc = cobraFakeDiscEject();
    if (rc != 0) return rc;

    rc = cobraMountPs3(files, 1);
    if (rc != 0) return rc;

    return cobraFakeDiscInsert();
}
