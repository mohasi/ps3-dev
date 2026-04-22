#pragma once

#include <stdint.h>

/* Resolved at load time by libvshtask_export_stub.a / libvshmain_export_stub.a */
extern int32_t  vshtask_A02D46E7(int32_t, const char *);
extern uint32_t vshmain_EB757101(void);

#define vshNotify(msg)  vshtask_A02D46E7(0, (msg))
#define isXmbReady()    (vshmain_EB757101() == 0)

/* LV2 syscall 837 — mounts /dev_blind, the writable mirror of /dev_flash
 * exposed by Cobra CFW (always on under EVILNAT). The Sony SDK ships no
 * system_call_N() macro, so we emit `sc` directly with PPU LV2 ABI:
 * r11 = syscall number, r3..r10 = args, result in r3. */
static inline int64_t mountDevBlind(void)
{
    register uint64_t r3  __asm__("3")  = (uint64_t)(uintptr_t)"CELL_FS_IOS:BUILTIN_FLSH1";
    register uint64_t r4  __asm__("4")  = (uint64_t)(uintptr_t)"CELL_FS_FAT";
    register uint64_t r5  __asm__("5")  = (uint64_t)(uintptr_t)"/dev_blind";
    register uint64_t r6  __asm__("6")  = 0;
    register uint64_t r7  __asm__("7")  = 0;
    register uint64_t r8  __asm__("8")  = 0;
    register uint64_t r9  __asm__("9")  = 0;
    register uint64_t r10 __asm__("10") = 0;
    register uint64_t r11 __asm__("11") = 837;

    __asm__ volatile ("sc\n"
        : "+r"(r3)
        : "r"(r4), "r"(r5), "r"(r6), "r"(r7),
          "r"(r8), "r"(r9), "r"(r10), "r"(r11)
        : "r0", "r12", "cr0", "ctr", "xer", "memory");
    return (int64_t)r3;
}
