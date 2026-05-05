#pragma once

/* Cobra syscall8 helpers used by the disc mount flow in this plugin. */

#include <stdint.h>
#include <sys/timer.h>
#include "dbg.h"

#define SC_COBRA                8
#define OP_FAKE_STORAGE_EVENT   0x7022
#define DEV_BDVD                0x101000000000006ULL
#define SUCCESS                 0

#define STORAGE_EVENT_PRE_EJECT     4
#define STORAGE_EVENT_EJECTED       8
#define STORAGE_EVENT_PRE_INSERT    7
#define STORAGE_EVENT_INSERTED      3

static inline int64_t scCall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    register uint64_t r3  __asm__("3")  = a1;
    register uint64_t r4  __asm__("4")  = a2;
    register uint64_t r5  __asm__("5")  = a3;
    register uint64_t r6  __asm__("6")  = a4;
    register uint64_t r11 __asm__("11") = num;
    __asm__ volatile ("sc\n" : "+r"(r3)
                      : "r"(r4), "r"(r5), "r"(r6), "r"(r11)
                      : "r0","r7","r8","r9","r10","r12",
                        "cr0","ctr","xer","memory");
    return (int64_t)r3;
}

/* Issue syscall8 opcode 0x7024 to switch Cobra's active emulated PS3 disc.
 * `files` points to a syscall-visible argv array of ISO part paths. */
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
    if (ret != SUCCESS) dbgLog("[sdm] sc8 mount_ps3 rc=0x%x\n", ret);
    return ret;
}

/* Raise a synthetic BD-drive storage event so XMB updates disc state.
 * The second syscall argument is always 0 for this flow. */
static inline int cobraFakeEvent(uint64_t event)
{
    sys_timer_usleep(5000);
    return (int)scCall4(SC_COBRA, OP_FAKE_STORAGE_EVENT, event, 0, DEV_BDVD);
}

static inline int cobraFakeDiscEject(void)
{
    int r = cobraFakeEvent(STORAGE_EVENT_PRE_EJECT);
    if (r != SUCCESS) {
        dbgLog("[sdm] pre-eject rc=0x%x\n", r);
        return r;
    }

    r = cobraFakeEvent(STORAGE_EVENT_EJECTED);
    if (r != SUCCESS) dbgLog("[sdm] eject rc=0x%x\n", r);
    return r;
}

static inline int cobraFakeDiscInsert(void)
{
    int r = cobraFakeEvent(STORAGE_EVENT_PRE_INSERT);
    if (r != SUCCESS) {
        dbgLog("[sdm] pre-insert rc=0x%x\n", r);
        return r;
    }

    r = cobraFakeEvent(STORAGE_EVENT_INSERTED);
    if (r != SUCCESS) dbgLog("[sdm] insert rc=0x%x\n", r);
    return r;
}

/* End-to-end mount flow: force XMB eject state, mount ISO via Cobra, then
 * publish insert state so the icon appears in the BD slot. */
static inline int cobraMountIso(const char *fullPath)
{
    char *files[1];
    files[0] = (char *)fullPath;

    int r = cobraFakeDiscEject();
    if (r != SUCCESS) {
        return r;
    }

    r = cobraMountPs3(files, 1);
    if (r != SUCCESS) {
        return r;
    }

    r = cobraFakeDiscInsert();
    if (r != SUCCESS) {
        return r;
    }

    return SUCCESS;
}
