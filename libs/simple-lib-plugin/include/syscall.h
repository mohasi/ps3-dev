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

static inline int64_t scCall5(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    register uint64_t r3  __asm__("3")  = a1;
    register uint64_t r4  __asm__("4")  = a2;
    register uint64_t r5  __asm__("5")  = a3;
    register uint64_t r6  __asm__("6")  = a4;
    register uint64_t r7  __asm__("7")  = a5;
    register uint64_t r11 __asm__("11") = num;
    __asm__ volatile ("sc\n" : "+r"(r3)
                      : "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r11)
                      : "r0","r8","r9","r10","r12",
                        "cr0","ctr","xer","memory");
    return (int64_t)r3;
}

static inline int64_t scCall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    register uint64_t r3  __asm__("3")  = a1;
    register uint64_t r4  __asm__("4")  = a2;
    register uint64_t r5  __asm__("5")  = a3;
    register uint64_t r6  __asm__("6")  = a4;
    register uint64_t r7  __asm__("7")  = a5;
    register uint64_t r8  __asm__("8")  = a6;
    register uint64_t r11 __asm__("11") = num;
    __asm__ volatile ("sc\n" : "+r"(r3)
                      : "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r8), "r"(r11)
                      : "r0","r9","r10","r12",
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

// prx introspection. caller-friendly wrappers around lv2 syscalls 494/495.
// the kernel's option-struct layouts are kept private inside the impls -
// callers just supply plain output buffers.
//
// references: rpcs3 sys_prx.h, Cobra-PS3 lv2/modules.h.
// the kernel checks opt->size == sizeof(struct) and rejects with EINVAL
// (0x80010002) if it doesn't, so the structs use natural alignment (no
// __attribute__((packed))) — trailing u64 padding is part of the abi.

#define PRX_NAME_MAX      30
#define PRX_FILENAME_MAX  512

// list prx ids loaded into the current process (vsh.self in our context).
// writes up to maxIds entries into ids[], stores actual count in *outCount.
// returns 0 on success, negative lv2 error otherwise.
static inline int32_t prxList(uint32_t *ids, uint32_t maxIds, uint32_t *outCount)
{
    struct {
        uint64_t size;
        uint32_t pad, max, count, idlist, unk;
    } opt;
    // kernel rejects opt.unk == NULL, but we never read it. local scratch.
    static uint32_t scratch[256];
    if (maxIds > sizeof scratch / sizeof scratch[0]) maxIds = sizeof scratch / sizeof scratch[0];

    opt.size   = sizeof opt;
    opt.pad    = 0;
    opt.max    = maxIds;
    opt.count  = 0;
    opt.idlist = (uint32_t)(uintptr_t)ids;
    opt.unk    = (uint32_t)(uintptr_t)scratch;

    int32_t rc = (int32_t)scCall2(494, 2, (uint64_t)(uintptr_t)&opt);
    *outCount = (rc < 0) ? 0 : opt.count;
    return rc;
}

// look up a prx by id; writes module name into nameOut[PRX_NAME_MAX] and
// (optionally) source file path into fileOut[PRX_FILENAME_MAX]. pass NULL
// for fileOut to skip the path. both buffers are always null-terminated
// on success. returns 0 or negative lv2 error.
static inline int32_t prxName(int32_t id, char *nameOut, char *fileOut)
{
    struct {
        uint64_t size;
        char     name[PRX_NAME_MAX];
        char     version[2];
        uint32_t modattribute, startEntry, stopEntry, allSegmentsNum;
        uint32_t filename, filenameSize, segments, segmentsNum;
    } info;
    struct {
        uint64_t size;
        uint32_t info;
        uint32_t pad;
    } opt;

    info.size         = sizeof info;
    info.filename     = (uint32_t)(uintptr_t)fileOut;
    info.filenameSize = fileOut ? PRX_FILENAME_MAX : 0;
    info.segments     = 0;
    info.segmentsNum  = 0;
    if (fileOut) fileOut[0] = '\0';

    opt.size = sizeof opt;
    opt.info = (uint32_t)(uintptr_t)&info;
    opt.pad  = 0;

    int32_t rc = (int32_t)scCall3(495, (uint64_t)id, 0, (uint64_t)(uintptr_t)&opt);
    if (rc < 0) { nameOut[0] = '\0'; return rc; }

    // syscall doesn't always null-terminate; force it.
    for (int i = 0; i < PRX_NAME_MAX; i++) nameOut[i] = info.name[i];
    nameOut[PRX_NAME_MAX - 1] = '\0';
    if (fileOut) fileOut[PRX_FILENAME_MAX - 1] = '\0';
    return 0;
}

