#pragma once

// generic ppu lv2 syscall trampolines and reusable lv2 wrappers.
//
// reference: https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls
// always cross-check syscall numbers / arg layouts against that page when
// adding or modifying anything in this file. the sony sdk does ship
// system_call_N() macros (sdk/.../sys/syscall.h); these trampolines are a
// faithful transcription of them so callers don't pull in PPU-only SDK headers.
// we emit `sc` directly using the ppu lv2 abi:
//   r11 = syscall number
//   r3..r10 = args
//   r3 = result
// keep these as static inline so callers pay no link cost and the file
// stays usable from any prx without pulling new objects into the build.
//
// this is the base layer in simple-lib-core, shared by apps and plugins.
// vsh-only NID stub exports (isXmbReady, vshNotify) live in
// simple-lib-plugin/vsh.h, not here.

#include <stdint.h>

// generic syscall trampolines. pick the one matching the arg count.
// mirrors the SDK <sys/syscall.h> system_call_1 (GCC form) exactly: `sc` writes
// r3..r11, so all are declared outputs; r3 (arg) and r11 (num) are the only inputs;
// the result is returned in r3. The wider output set + lr / cr1,5,6,7 clobbers tell
// the compiler `sc` can trash those, which the old compact form didn't.
static inline int64_t scCall1(uint64_t num, uint64_t a1)
{
   register uint64_t r3  __asm__("3")  = a1;
   register uint64_t r4  __asm__("4");
   register uint64_t r5  __asm__("5");
   register uint64_t r6  __asm__("6");
   register uint64_t r7  __asm__("7");
   register uint64_t r8  __asm__("8");
   register uint64_t r9  __asm__("9");
   register uint64_t r10 __asm__("10");
   register uint64_t r11 __asm__("11") = num;
   __asm__ volatile ("sc\n"
                     : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
                       "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
                     : "r"(r3), "r"(r11)
                     : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
   return (int64_t)r3;
}

static inline int64_t scCall2(uint64_t num, uint64_t a1, uint64_t a2)
{
   register uint64_t r3  __asm__("3")  = a1;
   register uint64_t r4  __asm__("4")  = a2;
   register uint64_t r5  __asm__("5");
   register uint64_t r6  __asm__("6");
   register uint64_t r7  __asm__("7");
   register uint64_t r8  __asm__("8");
   register uint64_t r9  __asm__("9");
   register uint64_t r10 __asm__("10");
   register uint64_t r11 __asm__("11") = num;
   __asm__ volatile ("sc\n"
                     : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
                       "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
                     : "r"(r3), "r"(r4), "r"(r11)
                     : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
   return (int64_t)r3;
}

static inline int64_t scCall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3)
{
   register uint64_t r3  __asm__("3")  = a1;
   register uint64_t r4  __asm__("4")  = a2;
   register uint64_t r5  __asm__("5")  = a3;
   register uint64_t r6  __asm__("6");
   register uint64_t r7  __asm__("7");
   register uint64_t r8  __asm__("8");
   register uint64_t r9  __asm__("9");
   register uint64_t r10 __asm__("10");
   register uint64_t r11 __asm__("11") = num;
   __asm__ volatile ("sc\n"
                     : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
                       "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
                     : "r"(r3), "r"(r4), "r"(r5), "r"(r11)
                     : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
   return (int64_t)r3;
}

static inline int64_t scCall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
   register uint64_t r3  __asm__("3")  = a1;
   register uint64_t r4  __asm__("4")  = a2;
   register uint64_t r5  __asm__("5")  = a3;
   register uint64_t r6  __asm__("6")  = a4;
   register uint64_t r7  __asm__("7");
   register uint64_t r8  __asm__("8");
   register uint64_t r9  __asm__("9");
   register uint64_t r10 __asm__("10");
   register uint64_t r11 __asm__("11") = num;
   __asm__ volatile ("sc\n"
                     : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
                       "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
                     : "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r11)
                     : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
   return (int64_t)r3;
}

static inline int64_t scCall5(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
   register uint64_t r3  __asm__("3")  = a1;
   register uint64_t r4  __asm__("4")  = a2;
   register uint64_t r5  __asm__("5")  = a3;
   register uint64_t r6  __asm__("6")  = a4;
   register uint64_t r7  __asm__("7")  = a5;
   register uint64_t r8  __asm__("8");
   register uint64_t r9  __asm__("9");
   register uint64_t r10 __asm__("10");
   register uint64_t r11 __asm__("11") = num;
   __asm__ volatile ("sc\n"
                     : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
                       "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
                     : "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r11)
                     : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
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
   register uint64_t r9  __asm__("9");
   register uint64_t r10 __asm__("10");
   register uint64_t r11 __asm__("11") = num;
   __asm__ volatile ("sc\n"
                     : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
                       "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
                     : "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r8), "r"(r11)
                     : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
   return (int64_t)r3;
}

static inline int64_t scCall7(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
   register uint64_t r3  __asm__("3")  = a1;
   register uint64_t r4  __asm__("4")  = a2;
   register uint64_t r5  __asm__("5")  = a3;
   register uint64_t r6  __asm__("6")  = a4;
   register uint64_t r7  __asm__("7")  = a5;
   register uint64_t r8  __asm__("8")  = a6;
   register uint64_t r9  __asm__("9")  = a7;
   register uint64_t r10 __asm__("10");
   register uint64_t r11 __asm__("11") = num;
   __asm__ volatile ("sc\n"
                     : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
                       "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
                     : "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r8), "r"(r9), "r"(r11)
                     : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
   return (int64_t)r3;
}

// lv2 syscall 837 - mount BUILTIN_FLSH1 (dev_flash) read-write at /dev_blind.
// idempotent -- a second call returns an error that is safe to ignore. kept as
// explicit asm rather than a trampoline because it deliberately zeroes r6..r10
// (the trailing mount args), which the generic scCallN forms leave undefined.
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

   // matches the SDK system_call_8 form: sc writes r3..r11 (all outputs); every arg
   // reg plus r11 is an input; full lr / cr1,5,6,7 clobbers.
   __asm__ volatile ("sc\n"
       : "=r"(r3), "=r"(r4), "=r"(r5), "=r"(r6),
         "=r"(r7), "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11)
       : "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7),
         "r"(r8), "r"(r9), "r"(r10), "r"(r11)
       : "r0","r12","lr","ctr","xer","cr0","cr1","cr5","cr6","cr7","memory");
   return (int64_t)r3;
}

// lv2 syscall 839 - sys_fs_sync(const char *device). forces every dirty buffer
// for the device -- file data, directory entries and the free-block bitmap --
// out to the platter, so the on-disk state and the XMB free-size match what the
// app just did and a power loss can't leave a half-updated volume. pass a device
// root like "/dev_hdd0" or "/dev_usb000" (see deviceRootOf in file.h). relies on
// the cobra fs syscalls being present. best-effort: callers ignore the return.
static inline int64_t syncDevice(const char *deviceRoot)
{
   return scCall1(839, (uint64_t)(uintptr_t)deviceRoot);
}

// true if the running kernel is PS3HEN rather than CFW/Cobra. Same syscall 8
// opcode answers on both -- cobra returns something other than 0x1337, HEN
// returns exactly that. Cached per translation unit: the answer can't change
// while running, and re-probing costs a syscall for nothing.
#define SYSCALL_COBRA        8
#define SYSCALL8_OP_IS_HEN   0x1337

static inline int isHenActive(void)
{
   static int cached = -1;
   if (cached < 0) cached = (scCall1(SYSCALL_COBRA, SYSCALL8_OP_IS_HEN) == 0x1337);
   return cached;
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

// cfw peek/poke of hypervisor (lv1) memory. syscall 11 is cobra's lv1 read,
// syscall 9 the cfw lv1 write. these reach below the operating system - a wrong
// address or value can hang the console outright, so only use them for values
// that are known-good from working cfw source.
static inline uint64_t peekLv1(uint64_t address)
{
   return (uint64_t)scCall1(11, address);
}

static inline void pokeLv1(uint64_t address, uint64_t value)
{
   (void)scCall2(9, address, value);
   // make the write visible to the hardware before anything else happens
   __asm__ volatile ("eieio\n sync\n" ::: "memory");
}

// thermal sensors. the console reports temperature per "zone": 0 = cell (cpu),
// 1 = rsx (gpu). the syscon thermal tables also mention zone 0x14 (southbridge)
// and 0x20 (voltage regulator), but hardware refuses both, so they are not here.
typedef enum ThermalZone {
   THERMAL_ZONE_CPU = 0,
   THERMAL_ZONE_RSX = 1
} ThermalZone;

// lv2 syscall 383 - sys_game_get_temperature. the raw value is fixed point:
// top byte = whole degrees celsius, next byte = 1/256ths. returns 0 on success.
static inline int32_t getConsoleTemperature(ThermalZone zone, uint32_t *outRaw)
{
   *outRaw = 0;
   return (int32_t)scCall2(383, (uint64_t)zone, (uint64_t)(uintptr_t)outRaw);
}

static inline int getTemperatureCelsius(uint32_t raw)      { return (int)(raw >> 24); }
static inline int getTemperatureTenths(uint32_t raw)       { return (int)(((raw >> 16) & 0xff) * 10 / 256); }

// what the fan controller is currently being driven by. duty is a pwm level,
// 0-255; there is no rpm reading anywhere in the system (the fan has no sense
// wire), so duty is the only fan number that exists.
typedef enum FanMode {
   FAN_MODE_FULL      = 0,
   FAN_MODE_AUTOMATIC = 1,   // syscon steps the duty up as the console heats
   FAN_MODE_MANUAL    = 2
} FanMode;

typedef struct FanPolicy {
   uint8_t status;
   uint8_t mode;
   uint8_t duty;
   uint8_t spare;
} FanPolicy;

// lv2 syscall 409 - sys_sm_get_fan_policy. gated behind the kernel's
// manufacturing-mode check on stock lv2; webman patches that check before
// calling. returns 0 on success, negative if refused.
static inline int32_t getFanPolicy(FanPolicy *outPolicy)
{
   outPolicy->status = outPolicy->mode = outPolicy->duty = outPolicy->spare = 0;
   return (int32_t)scCall5(409, 0, (uint64_t)(uintptr_t)&outPolicy->status, (uint64_t)(uintptr_t)&outPolicy->mode,
                                  (uint64_t)(uintptr_t)&outPolicy->duty, (uint64_t)(uintptr_t)&outPolicy->spare);
}

static inline int getFanPercent(uint8_t duty) { return (duty * 100 + 127) / 255; }

// lv2 syscall 867 - sys_ss_appliance_info_manager, packet 0x19003: the 16-byte
// console id the factory wrote into flash. its 8th byte is the product sub code,
// which names the motherboard and so the console model. returns 0 on success.
#define CONSOLE_ID_LENGTH 16

static inline int32_t getConsoleId(uint8_t *outConsoleId)
{
   return (int32_t)scCall2(867, 0x19003, (uint64_t)(uintptr_t)outConsoleId);
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
   // the kernel rejects opt.unk == NULL but never reads through it, and `opt`
   // itself is a per-call stack local, so this scratch is a true stack local too
   // (not a process-global) -- keeps prxList reentrant and matches its comment.
   uint32_t scratch[256];
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
