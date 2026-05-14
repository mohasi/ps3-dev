#pragma once

// generic ppu lv2 syscall trampolines and reusable lv2 wrappers.
//
// reference: https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls
// always cross-check syscall numbers / arg layouts against that page when
// adding or modifying anything in this file. the sony sdk ships no
// system_call_N() macros, so we emit `sc` directly using the ppu lv2 abi:
//   r11 = syscall number
//   r3..r10 = args
//   r3 = result
// keep these as static inline so callers pay no link cost and the file
// stays usable from any prx without pulling new objects into the build.

#include <stdint.h>

// generic syscall trampolines. pick the one matching the arg count.
static inline int64_t scCall1(uint64_t num, uint64_t a1)
{
    register uint64_t r3  __asm__("3")  = a1;
    register uint64_t r11 __asm__("11") = num;
    __asm__ volatile ("sc\n" : "+r"(r3)
                      : "r"(r11)
                      : "r0","r4","r5","r6","r7","r8","r9","r10","r12",
                        "cr0","ctr","xer","memory");
    return (int64_t)r3;
}

static inline int64_t scCall2(uint64_t num, uint64_t a1, uint64_t a2)
{
    register uint64_t r3  __asm__("3")  = a1;
    register uint64_t r4  __asm__("4")  = a2;
    register uint64_t r11 __asm__("11") = num;
    __asm__ volatile ("sc\n" : "+r"(r3)
                      : "r"(r4), "r"(r11)
                      : "r0","r5","r6","r7","r8","r9","r10","r12",
                        "cr0","ctr","xer","memory");
    return (int64_t)r3;
}

static inline int64_t scCall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t r3  __asm__("3")  = a1;
    register uint64_t r4  __asm__("4")  = a2;
    register uint64_t r5  __asm__("5")  = a3;
    register uint64_t r11 __asm__("11") = num;
    __asm__ volatile ("sc\n" : "+r"(r3)
                      : "r"(r4), "r"(r5), "r"(r11)
                      : "r0","r6","r7","r8","r9","r10","r12",
                        "cr0","ctr","xer","memory");
    return (int64_t)r3;
}

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

// lv2 syscall 837 - mounts /dev_blind, the writable mirror of /dev_flash
// exposed by cobra cfw (always on under evilnat).
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

// lv2 syscall 379 - sys_sm_shutdown. modes (per psdevwiki):
//   0x1100 = shutdown
//   0x1200 = lv2 hard reboot (full system + hv restart)
//   0x0200 = lv2 soft reboot (restarts lv2/vsh cleanly, hv stays up)
//   0x8201 = load lpar id 1 (rebug recovery — NOT a generic reboot)
static inline void sysPower(uint64_t mode)
{
    (void)scCall4(379, mode, 0, 0, 0);
}

// lv2 syscall 461 - sys_prx_get_module_id_by_address.
// returns the prx id of the module containing the given address.
static inline int32_t prxGetModuleIdByAddress(void *addr)
{
    return (int32_t)scCall1(461, (uint64_t)(uintptr_t)addr);
}

// lv2 syscall 482 - sys_prx_stop_module. used in prx _stop to tell lv2
// the module is finalized so its code pages can be safely unmapped
// after all threads have exited. pattern from webman-mod vsh_menu.
static inline void prxFinalizeSelf(void)
{
    static uint64_t meminfo[5] = { 0x28, 2, 0, 0, 0 };
    int32_t prx = prxGetModuleIdByAddress((void *)prxFinalizeSelf);
    (void)scCall3(482, (uint64_t)prx, 0, (uint64_t)(uintptr_t)meminfo);
}
