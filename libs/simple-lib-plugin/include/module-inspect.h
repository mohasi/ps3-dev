#pragma once

// PRX/main-exe linkage walkers. given the PT_LOAD segments of a loaded
// image, scan its address space for .lib.ent (export) and .lib.stub
// (import) records and emit one text row per record, plus one row per
// (nid, addr) pair. every pointer dereferenced is bounds-checked
// against the segment list first - records that don't validate are
// skipped rather than faulted on.
//
// the row tag names are passed in by the caller so the same walker can
// serve multiple wire formats (e.g. "ent"/"ef" for prx module-info vs.
// "pent"/"pef" for process-level main-exe linkage). this header owns
// the walking logic; callers own the protocol.

#include <stdint.h>
#include <stdio.h>
#include "syscall.h"   // for PrxSegment, PRX_SEGMENTS_MAX

#define INSPECT_NFUNC_MAX    4096   // sanity cap on per-lib function count
#define INSPECT_LIBNAME_MAX  64

// .lib.ent / .lib.stub record layouts - mirrored from sdk/sys/prx.h
// so we don't drag the PPU-only header into client code. NID tables
// and add tables are stored as runtime 32-bit virtual addresses
// inside the module's segment-0 mapping.
typedef struct {
   uint8_t  structsize;          // == 28
   uint8_t  reserved1;
   uint16_t version;
   uint16_t attribute;
   uint16_t nfunc;
   uint16_t nvar;
   uint16_t ntls;
   uint8_t  hashinfo, hashinfo2, reserved2, nidaltsets;
   uint32_t libname;
   uint32_t nidtable;
   uint32_t addtable;
} PrxLibEnt;

typedef struct {
   uint8_t  structsize;          // == 44
   uint8_t  reserved1;
   uint16_t version;
   uint16_t attribute;
   uint16_t nfunc;
   uint16_t nvar;
   uint16_t ntls;
   uint8_t  reserved2[4];
   uint32_t libname;
   uint32_t funcNidtable;
   uint32_t funcTable;
   uint32_t varNidtable;
   uint32_t varTable;
   uint32_t tlsNidtable;
   uint32_t tlsTable;
} PrxLibStub;

// returns 1 if [addr, addr+span) lies entirely inside any one segment.
// span==0 is treated as 1 (a single addressable byte).
static inline int addrInSegments(uint32_t addr, uint32_t span,
                                 const PrxSegment *segs, uint32_t segCount)
{
   if (addr == 0) return 0;
   if (span == 0) span = 1;
   uint32_t endAddr = addr + span;
   if (endAddr < addr) return 0; // overflow
   for (uint32_t i = 0; i < segCount && i < PRX_SEGMENTS_MAX; i++) {
      uint32_t base = (uint32_t)segs[i].base;
      uint32_t size = (uint32_t)segs[i].memsz;
      if (size == 0) continue;
      if (addr >= base && endAddr <= base + size) return 1;
   }
   return 0;
}

// copy a null-terminated string from a VA inside the module, with a
// hard byte cap. returns the number of bytes copied (excluding NUL).
// returns 0 if addr is unmapped or non-printable.
static inline uint32_t copyModuleString(char *out, uint32_t outCap, uint32_t addr,
                                        const PrxSegment *segs, uint32_t segCount)
{
   out[0] = '\0';
   if (!addrInSegments(addr, 1, segs, segCount)) return 0;
   const char *src = (const char *)(uintptr_t)addr;
   uint32_t i = 0;
   while (i < outCap - 1) {
      if (!addrInSegments(addr + i, 1, segs, segCount)) break;
      char c = src[i];
      if (c == '\0') break;
      if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) { out[0] = '\0'; return 0; }
      out[i] = c;
      i++;
   }
   out[i] = '\0';
   return i;
}

// emit one row per segment in the form
//   <tag>\t<i>\t<type>\t<base>\t<filesz>\t<memsz>\n
// callers pick the tag so the same emitter works for module-info
// ("seg") and process-info ("pseg"). returns updated off.
static inline uint32_t emitSegments(char *payload, uint32_t off, uint32_t cap,
                                    const PrxSegment *segs, uint32_t segCount,
                                    const char *tag)
{
   for (uint32_t i = 0; i < segCount; i++) {
      off += (uint32_t)snprintf(payload + off, cap - off,
                                "%s\t%llu\t0x%llx\t0x%llx\t0x%llx\t0x%llx\n",
                                tag,
                                (unsigned long long)segs[i].index,
                                (unsigned long long)segs[i].type,
                                (unsigned long long)segs[i].base,
                                (unsigned long long)segs[i].filesz,
                                (unsigned long long)segs[i].memsz);
   }
   return off;
}

// emit one libent (export) table to payload, validating every pointer
// against segs[] first. cursor walks [addr, addr+size). libTag/funcTag
// pick the wire format. returns updated off.
static inline uint32_t emitLibEnts(char *payload, uint32_t off, uint32_t cap,
                                   uint32_t addr, uint32_t size,
                                   const PrxSegment *segs, uint32_t segCount,
                                   const char *libTag, const char *funcTag,
                                   const char *varTag)
{
   uint32_t cursor = addr;
   uint32_t end    = addr + size;
   while (cursor + sizeof(PrxLibEnt) <= end && off + 256 < cap) {
      if (!addrInSegments(cursor, sizeof(PrxLibEnt), segs, segCount)) break;
      const PrxLibEnt *e = (const PrxLibEnt *)(uintptr_t)cursor;
      if (e->structsize == 0 || e->structsize > 64) break;
      uint16_t nfunc = e->nfunc;
      uint16_t nvar  = e->nvar;
      if (nfunc > INSPECT_NFUNC_MAX) nfunc = INSPECT_NFUNC_MAX;
      if (nvar  > INSPECT_NFUNC_MAX) nvar  = INSPECT_NFUNC_MAX;
      char libname[INSPECT_LIBNAME_MAX];
      copyModuleString(libname, sizeof libname, e->libname, segs, segCount);
      off += (uint32_t)snprintf(payload + off, cap - off,
                                "%s\t%s\tnfunc=%u\tnvar=%u\tntls=%u\n",
                                libTag, libname, e->nfunc, e->nvar, e->ntls);
      // libent has a single combined nid table laid out as
      // [func nids ...][var nids ...][tls nids ...]. addtable matches
      // 1:1 with nidtable. bounds-check the full range we plan to read
      // before dereferencing, then emit funcs first, vars second. addrs
      // are intentionally not emitted: trace uses funcTable, not export VAs.
      uint32_t total    = (uint32_t)nfunc + (uint32_t)nvar;
      uint32_t nidsAddr = e->nidtable;
      uint32_t addAddr  = e->addtable;
      if (addrInSegments(nidsAddr, total * 4, segs, segCount) &&
          addrInSegments(addAddr,  total * 4, segs, segCount)) {
         const uint32_t *nids = (const uint32_t *)(uintptr_t)nidsAddr;
         for (uint16_t i = 0; i < nfunc && off + 32 < cap; i++) {
            off += (uint32_t)snprintf(payload + off, cap - off,
                                      "%s\t0x%08x\n",
                                      funcTag, (unsigned)nids[i]);
         }
         if (varTag) {
            for (uint16_t i = 0; i < nvar && off + 32 < cap; i++) {
               off += (uint32_t)snprintf(payload + off, cap - off,
                                         "%s\t0x%08x\n",
                                         varTag, (unsigned)nids[nfunc + i]);
            }
         }
      }
      cursor += e->structsize;
   }
   return off;
}

// emit one libstub (import) table. mirrors emitLibEnts but for stubs.
static inline uint32_t emitLibStubs(char *payload, uint32_t off, uint32_t cap,
                                    uint32_t addr, uint32_t size,
                                    const PrxSegment *segs, uint32_t segCount,
                                    const char *libTag, const char *funcTag,
                                    const char *varTag)
{
   uint32_t cursor = addr;
   uint32_t end    = addr + size;
   while (cursor + sizeof(PrxLibStub) <= end && off + 256 < cap) {
      if (!addrInSegments(cursor, sizeof(PrxLibStub), segs, segCount)) break;
      const PrxLibStub *s = (const PrxLibStub *)(uintptr_t)cursor;
      if (s->structsize == 0 || s->structsize > 64) break;
      uint16_t nfunc = s->nfunc;
      uint16_t nvar  = s->nvar;
      if (nfunc > INSPECT_NFUNC_MAX) nfunc = INSPECT_NFUNC_MAX;
      if (nvar  > INSPECT_NFUNC_MAX) nvar  = INSPECT_NFUNC_MAX;
      char libname[INSPECT_LIBNAME_MAX];
      copyModuleString(libname, sizeof libname, s->libname, segs, segCount);
      off += (uint32_t)snprintf(payload + off, cap - off,
                                "%s\t%s\tnfunc=%u\tnvar=%u\tntls=%u\n",
                                libTag, libname, s->nfunc, s->nvar, s->ntls);
      // funcs and vars live in separate tables on the stub side. bounds-
      // check each pair before dereferencing. the funcTable VA is an
      // internal trace patching target and not host-facing.
      if (addrInSegments(s->funcNidtable, (uint32_t)nfunc * 4, segs, segCount) &&
          addrInSegments(s->funcTable,    (uint32_t)nfunc * 4, segs, segCount)) {
         const uint32_t *nids = (const uint32_t *)(uintptr_t)s->funcNidtable;
         for (uint16_t i = 0; i < nfunc && off + 32 < cap; i++) {
            off += (uint32_t)snprintf(payload + off, cap - off,
                                      "%s\t0x%08x\n",
                                      funcTag, (unsigned)nids[i]);
         }
      }
      if (varTag && nvar > 0 &&
          addrInSegments(s->varNidtable, (uint32_t)nvar * 4, segs, segCount) &&
          addrInSegments(s->varTable,    (uint32_t)nvar * 4, segs, segCount)) {
         const uint32_t *nids = (const uint32_t *)(uintptr_t)s->varNidtable;
         for (uint16_t i = 0; i < nvar && off + 32 < cap; i++) {
            off += (uint32_t)snprintf(payload + off, cap - off,
                                      "%s\t0x%08x\n",
                                      varTag, (unsigned)nids[i]);
         }
      }
      cursor += s->structsize;
   }
   return off;
}

// walk every (libname, nid, &funcTable[i], funcTable[i]) tuple in a
// libstub region and invoke `visit` for each. read-only; same bounds
// checks as emitLibStubs but no payload formatting. visit returns 0
// to stop the walk early, non-zero to keep going. used by the hook
// engine to enumerate patchable slots without touching them.
typedef int (*StubSlotVisitor)(const char *libname,
                               uint32_t nid,
                               uint32_t slotAddr,    // &funcTable[i]
                               uint32_t origValue,   // funcTable[i] before any patch
                               void    *userData);

static inline void forEachStubSlot(uint32_t addr, uint32_t size,
                                   const PrxSegment *segs, uint32_t segCount,
                                   StubSlotVisitor visit, void *userData)
{
   uint32_t cursor = addr;
   uint32_t end    = addr + size;
   while (cursor + sizeof(PrxLibStub) <= end) {
      if (!addrInSegments(cursor, sizeof(PrxLibStub), segs, segCount)) break;
      const PrxLibStub *s = (const PrxLibStub *)(uintptr_t)cursor;
      if (s->structsize == 0 || s->structsize > 64) break;
      uint16_t nfunc = s->nfunc;
      if (nfunc > INSPECT_NFUNC_MAX) nfunc = INSPECT_NFUNC_MAX;
      char libname[INSPECT_LIBNAME_MAX];
      copyModuleString(libname, sizeof libname, s->libname, segs, segCount);
      if (addrInSegments(s->funcNidtable, (uint32_t)nfunc * 4, segs, segCount) &&
          addrInSegments(s->funcTable,    (uint32_t)nfunc * 4, segs, segCount)) {
         const uint32_t *nids = (const uint32_t *)(uintptr_t)s->funcNidtable;
         const uint32_t *tab  = (const uint32_t *)(uintptr_t)s->funcTable;
         for (uint16_t i = 0; i < nfunc; i++) {
            uint32_t slotAddr = s->funcTable + (uint32_t)i * 4;
            if (!visit(libname, nids[i], slotAddr, tab[i], userData)) return;
         }
      }
      cursor += s->structsize;
   }
}

// walk every import-lib record in a libstub region and invoke `visit`
// once per (libname, nfunc). read-only. used by the trace dep resolver
// to discover which export libraries a module pulls in.
typedef int (*StubLibVisitor)(const char *libname, uint32_t nfunc, void *userData);

static inline void forEachStubLib(uint32_t addr, uint32_t size,
                                  const PrxSegment *segs, uint32_t segCount,
                                  StubLibVisitor visit, void *userData)
{
   uint32_t cursor = addr;
   uint32_t end    = addr + size;
   while (cursor + sizeof(PrxLibStub) <= end) {
      if (!addrInSegments(cursor, sizeof(PrxLibStub), segs, segCount)) break;
      const PrxLibStub *s = (const PrxLibStub *)(uintptr_t)cursor;
      if (s->structsize == 0 || s->structsize > 64) break;
      char libname[INSPECT_LIBNAME_MAX];
      copyModuleString(libname, sizeof libname, s->libname, segs, segCount);
      if (!visit(libname, s->nfunc, userData)) return;
      cursor += s->structsize;
   }
}

// walk every export-lib record in a libent region and invoke `visit`
// once per (libname, nfunc). read-only. used by the trace dep resolver
// to ask "does this loaded prx export anything from libname X?".
typedef int (*EntLibVisitor)(const char *libname, uint32_t nfunc, void *userData);

static inline void forEachEntLib(uint32_t addr, uint32_t size,
                                 const PrxSegment *segs, uint32_t segCount,
                                 EntLibVisitor visit, void *userData)
{
   uint32_t cursor = addr;
   uint32_t end    = addr + size;
   while (cursor + sizeof(PrxLibEnt) <= end) {
      if (!addrInSegments(cursor, sizeof(PrxLibEnt), segs, segCount)) break;
      const PrxLibEnt *e = (const PrxLibEnt *)(uintptr_t)cursor;
      if (e->structsize == 0 || e->structsize > 64) break;
      char libname[INSPECT_LIBNAME_MAX];
      copyModuleString(libname, sizeof libname, e->libname, segs, segCount);
      if (!visit(libname, e->nfunc, userData)) return;
      cursor += e->structsize;
   }
}

// scan PT_LOAD ranges for libstub/libent records that the loader has
// emitted but for which there is no sce_prx_param (main self images).
// for each candidate record at a 4-byte aligned offset, validate the
// structsize and that its libname / nid / addr / func tables fall
// inside known segments before emitting. records pointing outside
// segments are skipped; this is a read-only scan and must not fault.
// imports are scanned first so they're never truncated in favour of
// the (much larger) export list.
static inline uint32_t scanLinkage(char *payload, uint32_t off, uint32_t cap,
                                   const PrxSegment *segs, uint32_t segCount,
                                   const char *entTag,  const char *efTag, const char *evTag,
                                   const char *stubTag, const char *sfTag, const char *svTag)
{
   for (uint32_t i = 0; i < segCount; i++) {
      uint32_t base = (uint32_t)segs[i].base;
      uint32_t end  = base + (uint32_t)segs[i].filesz;
      if (end <= base) continue;
      for (uint32_t addr = base; addr + sizeof(PrxLibStub) <= end; addr += 4) {
         const PrxLibStub *s = (const PrxLibStub *)(uintptr_t)addr;
         if (s->structsize != sizeof(PrxLibStub)) continue;
         if (s->nfunc > INSPECT_NFUNC_MAX) continue;
         if (s->nvar  > INSPECT_NFUNC_MAX) continue;
         if (s->ntls  > INSPECT_NFUNC_MAX) continue;
         if (!addrInSegments(s->funcNidtable, (uint32_t)s->nfunc * 4, segs, segCount)) continue;
         if (!addrInSegments(s->funcTable,    (uint32_t)s->nfunc * 4, segs, segCount)) continue;
         char libname[INSPECT_LIBNAME_MAX];
         uint32_t nl = copyModuleString(libname, sizeof libname, s->libname, segs, segCount);
         if (s->libname != 0 && nl == 0) continue;
         off = emitLibStubs(payload, off, cap, addr, (uint32_t)s->structsize,
                            segs, segCount, stubTag, sfTag, svTag);
         addr += s->structsize - 4;
      }
   }
   for (uint32_t i = 0; i < segCount; i++) {
      uint32_t base = (uint32_t)segs[i].base;
      uint32_t end  = base + (uint32_t)segs[i].filesz;
      if (end <= base) continue;
      for (uint32_t addr = base; addr + sizeof(PrxLibEnt) <= end; addr += 4) {
         const PrxLibEnt *e = (const PrxLibEnt *)(uintptr_t)addr;
         if (e->structsize != sizeof(PrxLibEnt)) continue;
         if (e->nfunc > INSPECT_NFUNC_MAX) continue;
         if (e->nvar  > INSPECT_NFUNC_MAX) continue;
         if (e->ntls  > INSPECT_NFUNC_MAX) continue;
         if (!addrInSegments(e->nidtable, (uint32_t)e->nfunc * 4, segs, segCount)) continue;
         if (!addrInSegments(e->addtable, (uint32_t)e->nfunc * 4, segs, segCount)) continue;
         char libname[INSPECT_LIBNAME_MAX];
         uint32_t nl = copyModuleString(libname, sizeof libname, e->libname, segs, segCount);
         if (e->libname != 0 && nl == 0) continue;
         off = emitLibEnts(payload, off, cap, addr, (uint32_t)e->structsize,
                           segs, segCount, entTag, efTag, evTag);
         addr += e->structsize - 4;
      }
   }
   return off;
}

// build a PrxSegment[] list from PT_LOAD entries in the ELF64-BE image
// mapped at `elf`. used to give addrInSegments / scanLinkage a valid
// view of the main executable's address space when there's no
// sce_prx_param. returns the number of segments emitted, capped at
// `cap`. bytewise unpack to make endianness explicit and avoid
// depending on alignment of the mapped image.
//
// program header layout:
//   p_type   @ +0  (u32)   PT_LOAD = 1
//   p_vaddr  @ +16 (u64)
//   p_filesz @ +32 (u64)
//   p_memsz  @ +40 (u64)
static inline uint32_t loadElfSegments(const volatile uint8_t *elf,
                                       PrxSegment *segs, uint32_t cap)
{
   if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') return 0;
   uint64_t e_phoff = 0;
   for (int i = 0; i < 8; i++) e_phoff = (e_phoff << 8) | elf[32 + i];
   uint16_t e_phentsize = (uint16_t)((elf[54] << 8) | elf[55]);
   uint16_t e_phnum     = (uint16_t)((elf[56] << 8) | elf[57]);
   if (e_phnum > 16) e_phnum = 16;
   const volatile uint8_t *ph = elf + e_phoff;
   uint32_t n = 0;
   for (uint32_t i = 0; i < e_phnum && n < cap; i++) {
      const volatile uint8_t *p = ph + i * e_phentsize;
      uint32_t p_type = (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
      if (p_type != 1) continue;
      uint64_t vaddr = 0, filesz = 0, memsz = 0;
      for (int b = 0; b < 8; b++) vaddr  = (vaddr  << 8) | p[16 + b];
      for (int b = 0; b < 8; b++) filesz = (filesz << 8) | p[32 + b];
      for (int b = 0; b < 8; b++) memsz  = (memsz  << 8) | p[40 + b];
      if (vaddr == 0 || memsz == 0) continue;
      segs[n].index  = n;
      segs[n].type   = p_type;
      segs[n].base   = vaddr;
      segs[n].filesz = filesz;
      segs[n].memsz  = memsz;
      n++;
   }
   return n;
}
