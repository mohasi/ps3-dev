#include "export-hook.h"
#include "syscall.h"
#include "string-utilities.h"   // strEq
#include "game-mem.h"           // writeProcMem: the shared ps3mapi write path
#include "dbg.h"

#define TAG "[cht] "

#define SYS_PROCESS_GETPID  1   // vsh patches its OWN code, so writeCode writes to this pid

// vsh process layout: segment-15 base pointer and the export table offset
// within it (same constants the FPS-counter / Artemis detours use).
#define SEGMENT15_PTR_ADDR   0x1008C
#define EXPORT_TABLE_OFFSET  0x984
#define EXPORT_STUB_SSIZE    0x1C00   // marks a valid export-stub row

typedef struct { uint32_t func, toc; } Opd;

typedef struct {
   int16_t     ssize, header1, header2, exports;
   int32_t     zero1, zero2;
   const char *name;
   uint32_t   *fnid;
   Opd       **stub;
} ExportStub;

// globals the asm stub reads by absolute address (toc-independent), set
// before the entry is patched so the stub never sees stale values.
static uint32_t exportHookUserFn     = 0;   // code entry of the C hook
static uint32_t exportHookUserToc    = 0;   // our module's toc
static uint32_t exportHookTrampoline = 0;   // trampoline code address

// inbound stub. reached by a raw far-jump from the patched export entry, so
// r2 = callee toc and lr = original caller. save the inbound state, switch to
// our toc, call the C hook, restore, then run the original via the trampoline.
// f1/f2 are preserved because framework callbacks pass a float arg the
// original still needs.
extern uint32_t exportHookStub[];
__asm__(
   ".text\n"
   ".globl exportHookStub\n"
   "exportHookStub:\n"
   "   mflr  0\n"
   "   stdu  1, -0xD0(1)\n"
   "   std   0, 0xB8(1)\n"                    // original caller lr
   "   std   2, 0xB0(1)\n"                    // callee toc
   "   std   3, 0x70(1)\n"
   "   std   4, 0x78(1)\n"
   "   std   5, 0x80(1)\n"
   "   std   6, 0x88(1)\n"
   "   std   7, 0x90(1)\n"
   "   std   8, 0x98(1)\n"
   "   std   9, 0xA0(1)\n"
   "   std   10,0xA8(1)\n"
   "   stfd  1, 0xC0(1)\n"                    // float args frameTime etc.
   "   stfd  2, 0xC8(1)\n"
   "   lis   11, exportHookUserToc@ha\n"
   "   lwz   2, exportHookUserToc@l(11)\n"    // r2 = our toc
   "   lis   11, exportHookUserFn@ha\n"
   "   lwz   11, exportHookUserFn@l(11)\n"
   "   mtctr 11\n"
   "   bctrl\n"                               // call the C hook (void)
   "   lfd   2, 0xC8(1)\n"
   "   lfd   1, 0xC0(1)\n"
   "   ld    3, 0x70(1)\n"
   "   ld    4, 0x78(1)\n"
   "   ld    5, 0x80(1)\n"
   "   ld    6, 0x88(1)\n"
   "   ld    7, 0x90(1)\n"
   "   ld    8, 0x98(1)\n"
   "   ld    9, 0xA0(1)\n"
   "   ld    10,0xA8(1)\n"
   "   ld    2, 0xB0(1)\n"                    // restore callee toc
   "   ld    0, 0xB8(1)\n"
   "   mtlr  0\n"
   "   addi  1, 1, 0xD0\n"
   "   lis   11, exportHookTrampoline@ha\n"
   "   lwz   11, exportHookTrampoline@l(11)\n"
   "   mtctr 11\n"
   "   bctr\n"                                // run the original
   // trampoline storage, reserved as code so it is genuinely executable and
   // fetch-aligned; written via the debug syscall at install. worst case =
   // 4 displaced branches (6 instrs each) + jump-back (6) = 120 bytes.
   "   .align 4\n"
   ".globl exportHookTrampolineBuf\n"
   "exportHookTrampolineBuf:\n"
   "   .space 256\n"
);
extern uint8_t exportHookTrampolineBuf[];

// our module's toc = current r2 while we run our own code.
static uint32_t getMyToc(void)
{
   uint32_t toc;
   __asm__ volatile ("mr %0, 2" : "=r"(toc));
   return toc;
}

static Opd *findExport(const char *module, uint32_t fnid)
{
   uint32_t   *segment15 = *(uint32_t **)(uintptr_t)SEGMENT15_PTR_ADDR;
   ExportStub *stub      = (ExportStub *)(uintptr_t)segment15[EXPORT_TABLE_OFFSET / sizeof(uint32_t)];

   while (stub->ssize == EXPORT_STUB_SSIZE) {
      if (strEq(module, stub->name))
         for (int16_t i = 0; i < stub->exports; i++)
            if (stub->fnid[i] == fnid) return stub->stub[i];
      stub++;
   }
   return 0;
}

static int writeCode(void *dst, const void *src, uint32_t size)
{
   uint32_t pid = (uint32_t)scCall1(SYS_PROCESS_GETPID, 0);
   int rc = writeProcMem(pid, (uint32_t)(uintptr_t)dst, src, size);
   logInfo(TAG "writeCode rc=0x%x\n", (unsigned)rc);
   return rc;
}

// ppc instruction encoders (reg = GPR index, disp = signed 16, multiple of 4).
#define PPC_LIS(reg, imm)      (0x3C000000u | ((uint32_t)(reg) << 21) | ((uint32_t)(imm) & 0xFFFF))
#define PPC_ORI(reg, imm)      (0x60000000u | ((uint32_t)(reg) << 21) | ((uint32_t)(reg) << 16) | ((uint32_t)(imm) & 0xFFFF))
#define PPC_MTCTR(reg)         (0x7C0903A6u | ((uint32_t)(reg) << 21))
#define PPC_BCCTR(bo, bi, lk)  (0x4C000420u | ((uint32_t)(bo) << 21) | ((uint32_t)(bi) << 16) | ((uint32_t)(lk) & 1))
#define PPC_STD(reg, ra, disp) (0xF8000000u | ((uint32_t)(reg) << 21) | ((uint32_t)(ra) << 16) | ((uint32_t)(disp) & 0xFFFC))
#define PPC_LD(reg, ra, disp)  (0xE8000000u | ((uint32_t)(reg) << 21) | ((uint32_t)(ra) << 16) | ((uint32_t)(disp) & 0xFFFC))

#define PPC_OP_B   0x48000000u   // b/ba/bl/bla
#define PPC_OP_BC  0x40000000u   // bc and its variants

// emit a far branch to target via ctr. bo/bi select the condition (bo=20 =
// unconditional), linked = call. preserve saves/restores the scratch reg
// through the red zone so a relocated in-body branch clobbers nothing.
// returns the number of instructions written.
static int emitBranch(uint32_t *out, uint32_t target, uint32_t bo, uint32_t bi,
                      int linked, int preserve, uint32_t reg)
{
   int n = 0;
   if (preserve) out[n++] = PPC_STD(reg, 1, -0x30);
   out[n++] = PPC_LIS(reg, target >> 16);
   out[n++] = PPC_ORI(reg, target & 0xFFFF);
   out[n++] = PPC_MTCTR(reg);
   if (preserve) out[n++] = PPC_LD(reg, 1, -0x30);
   out[n++] = PPC_BCCTR(bo, bi, linked);
   return n;
}

// copy one instruction into the trampoline, rewriting pc-relative branches
// to absolute far branches (their offset is relative to the original site).
// everything else — normal instructions and register-indirect branches
// (bctr/blr) — is position-independent and copied verbatim. returns the
// number of instructions written.
static int relocateInstr(uint32_t *out, uint32_t srcAddr, uint32_t instr)
{
   uint32_t op = instr & 0xFC000000u;
   if ((op != PPC_OP_B && op != PPC_OP_BC) || (instr & 0x2)) {
      out[0] = instr;                          // position-independent or absolute
      return 1;
   }

   uint32_t bo, bi;
   int bits;
   if (op == PPC_OP_B) { bits = 24; bo = 20; bi = 0; }
   else                { bits = 14; bo = (instr >> 21) & 0x1F; bi = (instr >> 16) & 0x1F; }

   int32_t offset = (int32_t)(instr & (((1u << bits) - 1u) << 2));
   if (offset & (1 << (bits + 1)))             // sign-extend from the top offset bit
      offset |= ~(int32_t)(((1u << (bits + 2)) - 1u));

   return emitBranch(out, srcAddr + (uint32_t)offset, bo, bi, instr & 1, 1, 0);
}

int installExportHook(const char *libName, uint32_t fnid, void (*hookFn)(void))
{
   Opd *target = findExport(libName, fnid);
   if (!target) { logError(TAG "hook: export %s/0x%x not found\n", libName, fnid); return -1; }

   uint32_t  entry     = target->func;
   uint32_t *entryCode = (uint32_t *)(uintptr_t)entry;
   logInfo(TAG "hook prologue @0x%x: %08x %08x %08x %08x\n",
           entry, entryCode[0], entryCode[1], entryCode[2], entryCode[3]);

   // trampoline = the 4 displaced instructions (branches relocated) followed
   // by an unconditional jump back to entry+16, preserving r11.
   uint32_t tramp[64];
   int n = 0;
   for (int i = 0; i < 4; i++) n += relocateInstr(&tramp[n], entry + (uint32_t)i * 4, entryCode[i]);
   n += emitBranch(&tramp[n], entry + 16, 20, 0, 0, 1, 11);
   if (writeCode(exportHookTrampolineBuf, tramp, (uint32_t)n * 4) != 0) {
      logError(TAG "hook: trampoline write failed\n");
      return -3;
   }

   // publish what the stub reads before redirecting the entry to it.
   exportHookTrampoline = (uint32_t)(uintptr_t)exportHookTrampolineBuf;
   exportHookUserToc    = getMyToc();
   exportHookUserFn     = ((uint32_t *)(uintptr_t)hookFn)[0];   // opd -> code entry
   __sync_synchronize();

   // patch the entry with an unconditional far jump to our stub (4 instrs).
   uint32_t jump[4];
   (void)emitBranch(jump, (uint32_t)(uintptr_t)exportHookStub, 20, 0, 0, 0, 11);
   if (writeCode(entryCode, jump, sizeof(jump)) != 0) {
      logError(TAG "hook: entry patch failed\n");
      return -4;
   }

   logInfo(TAG "hook installed %s/0x%x entry=0x%x stub=0x%x tramp=0x%x\n",
           libName, fnid, entry, (uint32_t)(uintptr_t)exportHookStub, exportHookTrampoline);
   return 0;
}
