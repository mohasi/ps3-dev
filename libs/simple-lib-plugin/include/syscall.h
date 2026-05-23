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
static inline int32_t getPrxModuleIdByAddress(void *addr)
{
    return (int32_t)scCall1(461, (uint64_t)(uintptr_t)addr);
}

// lv2 syscall 482 - sys_prx_stop_module. used in prx _stop to tell lv2
// the module is finalized so its code pages can be safely unmapped
// after all threads have exited. pattern from webman-mod vsh_menu.
static inline void prxFinalizeSelf(void)
{
    static uint64_t meminfo[5] = { 0x28, 2, 0, 0, 0 };
    int32_t prx = getPrxModuleIdByAddress((void *)prxFinalizeSelf);
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
#define PRX_SEGMENTS_MAX  4   // SYS_MODULE_MAX_SEGMENTS from sys/moduleexport.h

// one ELF PT_LOAD segment as reported by sys_prx_get_module_info. base is
// the runtime virtual address the segment was mapped to (this is what we
// need to walk imports/exports — segment 0 is the RX code+rodata segment
// that holds sce_module_info, _scelibstub[], _scelibent[]).
typedef struct {
    uint64_t base;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t index;
    uint64_t type;
} PrxSegment;

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

// look up a prx by id.
//   nameOut    : PRX_NAME_MAX bytes, always null-terminated on success.
//   fileOut    : PRX_FILENAME_MAX bytes, source path, null-terminated.
//                MUST be a real caller-owned buffer even if you don't
//                care about the value — see quirk below.
//   segsOut    : array of PrxSegment, filled up to min(segsMax, allSegmentsNum).
//                may be NULL to skip.
//   segsMax    : capacity of segsOut. ignored if segsOut is NULL.
//   segsCount  : actual segment count reported by the kernel (can exceed segsMax).
// returns 0 or negative lv2 error.
//
// historical notes (resolved):
//   - the kernel does not always null-terminate info.name; we zero the
//     buffer before each call so trailing stale bytes can't survive into
//     a strEq with a shorter name.
//   - fileOut must be a real caller-owned buffer with non-zero
//     filenameSize even if the path is uninteresting — the kernel only
//     fills the name field reliably when filename is also valid.
//   - the original "pad[16]" guard turned out to be the four v2-extension
//     fields (libent_addr/size, libstub_addr/size). passing
//     sizeof(info) == 88 selects the v2 layout; the kernel writes them
//     unconditionally. exposed via prxLinkage() below.
static inline int32_t prxInfo(int32_t id, char *nameOut, char *fileOut,
                              PrxSegment *segsOut, uint32_t segsMax, uint32_t *segsCount)
{
    // sys_prx_module_info_v2_t layout. sizeof == 88 selects v2 in lv2.
    // declared in sdk/.../sys/prx.h; we mirror it here because the SDK
    // header pulls in PPU-only typedefs we don't want everywhere.
    struct {
        uint64_t size;
        char     name[PRX_NAME_MAX];
        char     version[2];
        uint32_t modattribute, startEntry, stopEntry, allSegmentsNum;
        uint32_t filename, filenameSize, segments, segmentsNum;
        uint32_t libentAddr, libentSize, libstubAddr, libstubSize;
    } info;
    struct {
        uint64_t size;
        uint32_t info;
        uint32_t pad;
    } opt;

    for (uint32_t i = 0; i < sizeof info; i++) ((char *)&info)[i] = 0;
    info.size         = sizeof info;
    info.filename     = (uint32_t)(uintptr_t)fileOut;
    info.filenameSize = PRX_FILENAME_MAX;
    info.segments     = (uint32_t)(uintptr_t)segsOut;
    info.segmentsNum  = segsOut ? segsMax : 0;
    fileOut[0] = '\0';

    opt.size = sizeof opt;
    opt.info = (uint32_t)(uintptr_t)&info;
    opt.pad  = 0;

    int32_t rc = (int32_t)scCall3(495, (uint64_t)id, 0, (uint64_t)(uintptr_t)&opt);
    if (rc < 0) {
        if (nameOut) nameOut[0] = '\0';
        if (segsCount) *segsCount = 0;
        return rc;
    }

    if (nameOut) {
        for (int i = 0; i < PRX_NAME_MAX; i++) nameOut[i] = info.name[i];
        nameOut[PRX_NAME_MAX - 1] = '\0';
    }
    fileOut[PRX_FILENAME_MAX - 1] = '\0';
    if (segsCount) *segsCount = info.allSegmentsNum;
    return 0;
}

// thin wrapper: name + filename, no segments. callers that don't need
// the path still must allocate the buffer; the kernel relies on it.
static inline int32_t prxName(int32_t id, char *nameOut, char *fileOut)
{
    return prxInfo(id, nameOut, fileOut, 0, 0, 0);
}

// .lib.ent / .lib.stub table descriptors for one loaded prx, as
// reported by the v2 form of sys_prx_get_module_info. addresses are
// runtime VAs inside the module's segment-0 mapping; sizes are byte
// lengths of arrays of sys_prx_libent32_t / sys_prx_libstub32_t (the
// individual records carry their own structsize, so divide carefully).
typedef struct {
    uint32_t libentAddr;
    uint32_t libentSize;
    uint32_t libstubAddr;
    uint32_t libstubSize;
} PrxLinkage;

// pull the export (.lib.ent) and import (.lib.stub) table descriptors
// for a loaded prx, plus its segment table so callers can validate
// pointers before dereferencing them. populated unconditionally by the
// v2 syscall path.
static inline int32_t prxLinkage(int32_t id, PrxLinkage *outLink,
                                 PrxSegment *segsOut, uint32_t segsMax,
                                 uint32_t *segsCount)
{
    struct {
        uint64_t size;
        char     name[PRX_NAME_MAX];
        char     version[2];
        uint32_t modattribute, startEntry, stopEntry, allSegmentsNum;
        uint32_t filename, filenameSize, segments, segmentsNum;
        uint32_t libentAddr, libentSize, libstubAddr, libstubSize;
    } info;
    struct { uint64_t size; uint32_t info; uint32_t pad; } opt;
    char file[PRX_FILENAME_MAX];
    for (uint32_t i = 0; i < sizeof info; i++) ((char *)&info)[i] = 0;
    info.size         = sizeof info;
    info.filename     = (uint32_t)(uintptr_t)file;
    info.filenameSize = PRX_FILENAME_MAX;
    info.segments     = (uint32_t)(uintptr_t)segsOut;
    info.segmentsNum  = segsOut ? segsMax : 0;
    file[0]           = '\0';
    opt.size = sizeof opt;
    opt.info = (uint32_t)(uintptr_t)&info;
    opt.pad  = 0;
    int32_t rc = (int32_t)scCall3(495, (uint64_t)id, 0, (uint64_t)(uintptr_t)&opt);
    if (rc < 0) {
        if (outLink) { outLink->libentAddr = outLink->libentSize = outLink->libstubAddr = outLink->libstubSize = 0; }
        if (segsCount) *segsCount = 0;
        return rc;
    }
    if (outLink) {
        outLink->libentAddr  = info.libentAddr;
        outLink->libentSize  = info.libentSize;
        outLink->libstubAddr = info.libstubAddr;
        outLink->libstubSize = info.libstubSize;
    }
    if (segsCount) *segsCount = info.allSegmentsNum;
    return 0;
}

// lv2 syscalls 348/349 - sys_memory_allocate / sys_memory_free. used by
// plugins that need scratch larger than their BSS budget (e.g. module-hook
// event ring + per-arm slot tables) without bloating the on-disk SPRX and
// trampling neighboring flash-mapped regions like cobra. flags use
// SYS_MEMORY_PAGE_SIZE_{1M,64K} from <sys/memory.h>; pageFlag below names
// the two common cases. size is rounded up to the page granularity by lv2.
#define SYS_PAGE_64K  0x200ULL
#define SYS_PAGE_1M   0x400ULL

static inline int32_t sysMemAllocate(uint32_t size, uint64_t pageFlag, uint32_t *outAddr)
{
    uint32_t addr = 0;
    int32_t  rc   = (int32_t)scCall3(348, (uint64_t)size, pageFlag,
                                     (uint64_t)(uintptr_t)&addr);
    *outAddr = (rc < 0) ? 0 : addr;
    return rc;
}

static inline int32_t sysMemFree(uint32_t addr)
{
    return (int32_t)scCall1(349, (uint64_t)addr);
}

