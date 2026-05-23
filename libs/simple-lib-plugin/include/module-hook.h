#pragma once

// outgoing-call trace engine for VSH PRX modules.
//
// each import of a PRX has a loader-built OPD `{entry, toc}` in a
// stub-OPD table; the importer's funcTable[i] holds the address of
// that OPD and is re-read per call by the loader stub thunks. we
// replace funcTable[i] with the address of a per-slot fake OPD
// `{hookTrampolineCode, &slot->ctx}`. the trampoline records the
// call into a ring then tail-calls the real entry with the original
// toc, so the callee runs unchanged.
//
// memory layout: ONE 64KB heap block covers everything an armed
// session owns. vsh-prx allocations must stay in the double-digit
// KB range, so we cannot stack multiple 64KB pages back-to-back -
// the second one gets refused. layout inside the block:
//
//   [0          .. RING_CTRL_BYTES)      ring control header (writeIdx, readIdx, mask, pad)
//   [RING_CTRL  .. RING_CTRL + RING)     event ring (RING bytes, RING/16 events)
//   [after ring .. + modsBytes)          HookMod[] table
//   [after mods .. + slotsBytes)         HookSlot[] table
//   [..end of block)                     unused tail
//
// each event is 16 bytes: { slotAddr, r3, r4, r5 } captured at call
// site. slotAddr identifies (module, nid) via the manifest; r3..r5
// give us `this` + the first two args, which is enough to follow
// paf::SyncCall callback addresses, library handles, etc.
//
// disarm frees the whole block. nothing else holds hook memory.

#include <stdint.h>
#include <stdio.h>
#include "syscall.h"        // prxInfo, prxLinkage, PrxSegment, sysMemAllocate
#include "module-inspect.h" // forEachStubSlot, addrInSegments
#include "dbg.h"

// single-allocation budget. one 64KB lv2 page; no second alloc.
#define HOOK_ARENA_BYTES   (64u * 1024u)

// ring control header. lives at offset 0 of the arena so the trampoline
// can load `writeIdx` / `ringBase` / `ringMask` from a single global
// pointer (`hookRingCtrl`), not per-slot fields. shaves 12 bytes off
// every HookSlot.
//
// layout consumed by the asm trampoline (offsets fixed):
//   0: writeIdx  4: ringBase  8: ringMask  12: readIdx (drain-side only)
typedef struct {
    uint32_t writeIdx;   // bumped by trampoline, ANDed with mask for store
    uint32_t ringBase;   // == arena + RING_CTRL_BYTES
    uint32_t ringMask;   // entry-count mask (ringSize-1)
    uint32_t readIdx;    // drain cursor (consumer-only)
} HookRingCtrl;

#define HOOK_RING_CTRL_BYTES  ((uint32_t)sizeof(HookRingCtrl))

// ring sized for the worst burst between 20ms drains. 8KB / 16B per
// event = 512 events; observed nas peak ~800 events / 20ms hits the
// edge, so a hot burst may drop - reported via hookDropCount(). cut
// the drain period to 10ms first if it ever fires.
#define HOOK_RING_BYTES   (8u * 1024u)
#define HOOK_EVENT_BYTES  16u
#define HOOK_EVENT_WORDS  (HOOK_EVENT_BYTES / 4u)
#define HOOK_RING_SIZE    (HOOK_RING_BYTES / HOOK_EVENT_BYTES)
#define HOOK_RING_MASK    (HOOK_RING_SIZE - 1u)

// global pointer the trampoline + drain use. set by allocHookArm,
// cleared by freeHookArm. trampoline reads writeIdx, ringBase, ringMask
// from here once per call.
static HookRingCtrl *hookRingCtrl = 0;

// shared trampoline. caller's bctr lands here with r2 = &slot->ctx,
// where ctx = {origEntry, origToc, eventTag}. ring control is read via
// the global hookRingCtrl pointer (linker-resolved address loaded into
// r11), so the per-slot ctx stays at 12 bytes.
//
// each event is 4 words: { slotAddr, r3, r4, r5 }. r3..r5 are captured
// straight off the inbound argument registers before we touch anything,
// giving us `this` + first two args for every call. scratch is r0/r11/
// r12 only; r3..r10 reach the callee untouched.
//
// store-then-bump: event is written at OLD writeIdx, then writeIdx is
// committed to old+1. avoids the ring[0] phantom event the earlier
// bump-then-store variant produced.
//
// HookContext layout (offsets fixed):
//   0: origEntry  4: origToc  8: eventTag
//
// declared as uint32[] so the symbol resolves to the label address,
// not a function OPD; we need the raw code address as fakeOpd.entry.
extern uint32_t hookTrampolineCode[];
__asm__(
    ".section .text\n"
    ".globl hookTrampolineCode\n"
    "hookTrampolineCode:\n"
    "    lis   11, hookRingCtrl@ha\n"     // r11 hi = &hookRingCtrl
    "    lwz   11, hookRingCtrl@l(11)\n"  // r11 = hookRingCtrl
    "    lwz   12, 0(11)\n"               // r12 = oldWrite
    "    addi   0, 12, 1\n"               // r0  = oldWrite + 1
    "    stw    0, 0(11)\n"               // writeIdx = oldWrite + 1
    "    lwz    0, 8(11)\n"               // r0  = ringMask
    "    and   12, 12, 0\n"               // r12 = oldWrite & ringMask
    "    slwi  12, 12, 4\n"               // r12 = byte offset (16B stride)
    "    lwz   11, 4(11)\n"               // r11 = ringBase (ctrl no longer needed)
    "    add   11, 11, 12\n"              // r11 = &ring[(oldWrite & mask) * 16]
    "    lwz   12, 8(2)\n"                // r12 = eventTag (slotAddr)
    "    stw   12, 0(11)\n"               // ev[0] = slotAddr
    "    stw    3, 4(11)\n"               // ev[1] = r3
    "    stw    4, 8(11)\n"               // ev[2] = r4
    "    stw    5,12(11)\n"               // ev[3] = r5
    "    lwz   12, 0(2)\n"                // r12 = origEntry
    "    lwz   11, 4(2)\n"                // r11 = origToc
    "    mtctr 12\n"
    "    mr     2, 11\n"                  // r2 = origToc
    "    bctr\n"
);

// per-slot trampoline context. only the truly per-slot fields. shared
// ring-control fields (writeIdxAddr/ringBase/ringMask) are hoisted to
// the global HookRingCtrl loaded by hookRingCtrl above.
typedef struct {
    uint32_t origEntry;
    uint32_t origToc;
    uint32_t eventTag;   // == slotAddr; manifest re-maps to (mod, nid)
} HookContext;

// fake OPD published into funcTable[i]. 8-byte aligned so a caller
// doing `ld r12,0(r11); ld r2,8(r11)` sees a coherent pair.
typedef struct __attribute__((aligned(8))) {
    uint32_t trampEntry;
    uint32_t ctxAddr;
} HookOpd;

// per-slot record. 32 bytes (8-aligned). nid is kept inline so the
// manifest writer doesn't have to re-walk libstubs at disarm time;
// modIdx is dropped (recovered by binary-searching mods[].first).
typedef struct __attribute__((aligned(8))) {
    HookOpd     fakeOpd;     // funcTable[i] will point here after arm (8B)
    HookContext ctx;         // trampoline reads via r2          (12B)
    uint32_t    slotAddr;    // &funcTable[i] inside target seg1 (4B)
    uint32_t    origValue;   // original funcTable[i]            (4B)
    uint32_t    nid;         // for manifest                     (4B)
} HookSlot;                  // total: 32B (padded from 32 - no slack)

// one row per armed module: contiguous run of slots[first..first+count).
// id is kept so disarm can re-query the module's current segments and
// skip writes into pages that have been unloaded or relocated.
typedef struct {
    uint32_t first;
    uint32_t count;
    int32_t  id;                   // sys_prx id, for live-segment re-check at disarm
    char     name[PRX_NAME_MAX];   // PRX_NAME_MAX == 30
} HookMod;

// everything an armed session owns. all pointers index into the single
// arena block (arenaAddr); freeHookArm releases it and zeroes state.
typedef struct {
    uint32_t  arenaAddr;     // sysMemFree handle for the 64KB block
    HookMod  *mods;          // arena + ring header + ring bytes
    HookSlot *slots;         // arena + ... + modsBytes
    uint32_t  slotCap;       // max slots that fit in this arena
    uint32_t  modCap;        // max mods that fit in this arena
    uint32_t  slotCursor;    // append position; final == armed count
    uint32_t  modCursor;     // same, for mods[]
    uint32_t  slotsRequested;// total slots the caller wanted to arm
    uint32_t  slotsDropped;  // slotsRequested - slotCursor (capacity miss)
} HookArm;

static HookArm activeArm;

static inline int isHookArmed(void) { return activeArm.arenaAddr != 0; }

// truncation telemetry: how many requested slots didn't fit. zero on a
// clean arm.
static inline uint32_t hookSlotsRequested(void) { return activeArm.slotsRequested; }
static inline uint32_t hookSlotsArmed(void)     { return activeArm.slotCursor; }
static inline uint32_t hookSlotsDropped(void)   { return activeArm.slotsDropped; }

// allocate the single 64KB arena and lay out ring header, mods[], slots[]
// inside it. `requestedSlots` is the caller's pre-count of all import
// slots across {root, deps...} - it sets only slotsRequested for the
// truncation report; the actual cap comes from arena geometry.
// rc<0:
//   -1 arena alloc failed   -3 already armed
static inline int allocHookArm(uint32_t requestedSlots, uint32_t modCount)
{
    if (isHookArmed()) return -3;
    if (modCount == 0) return -3;

    uint32_t arenaAddr = 0;
    int32_t  rc = sysMemAllocate(HOOK_ARENA_BYTES, SYS_PAGE_64K, &arenaAddr);
    logInfo("[hook] arena alloc rc=0x%x addr=0x%x bytes=%u\n",
            (unsigned)rc, (unsigned)arenaAddr, (unsigned)HOOK_ARENA_BYTES);
    if (rc < 0 || arenaAddr == 0) {
        logError("[hook] arena alloc failed rc=0x%x\n", (unsigned)rc);
        return -1;
    }

    // layout: [ring ctrl][ring bytes][mods[modCount]][slots[capSlots]]
    uint32_t modsBytes = modCount * (uint32_t)sizeof(HookMod);
    uint32_t fixed     = HOOK_RING_CTRL_BYTES + HOOK_RING_BYTES + modsBytes;
    if (fixed >= HOOK_ARENA_BYTES) {
        // too many mods requested for one arena. shouldn't happen in
        // practice (modCount capped well below this), but fail clean.
        logError("[hook] arena too small for modCount=%u (fixed=%u)\n",
                 (unsigned)modCount, (unsigned)fixed);
        sysMemFree(arenaAddr);
        return -1;
    }
    uint32_t slotsRoom = HOOK_ARENA_BYTES - fixed;
    uint32_t slotCap   = slotsRoom / (uint32_t)sizeof(HookSlot);

    HookRingCtrl *ctrl = (HookRingCtrl *)(uintptr_t)arenaAddr;
    ctrl->writeIdx = 0;
    ctrl->ringBase = arenaAddr + HOOK_RING_CTRL_BYTES;
    ctrl->ringMask = HOOK_RING_MASK;
    ctrl->readIdx  = 0;

    // wipe ring so a fresh arm starts with a clean window. ring is
    // (HOOK_RING_SIZE * HOOK_EVENT_WORDS) words of 4 bytes.
    uint32_t *ring = (uint32_t *)(uintptr_t)ctrl->ringBase;
    for (uint32_t r = 0; r < HOOK_RING_SIZE * HOOK_EVENT_WORDS; r++) ring[r] = 0;

    activeArm.arenaAddr      = arenaAddr;
    activeArm.mods           = (HookMod  *)(uintptr_t)(arenaAddr + HOOK_RING_CTRL_BYTES + HOOK_RING_BYTES);
    activeArm.slots          = (HookSlot *)(uintptr_t)(arenaAddr + HOOK_RING_CTRL_BYTES + HOOK_RING_BYTES + modsBytes);
    activeArm.slotCap        = slotCap;
    activeArm.modCap         = modCount;
    activeArm.slotCursor     = 0;
    activeArm.modCursor      = 0;
    activeArm.slotsRequested = requestedSlots;
    activeArm.slotsDropped   = 0;

    hookRingCtrl = ctrl;

    logInfo("[hook] arena=0x%x ring=0x%x mods=0x%x slots=0x%x slotCap=%u modCap=%u requested=%u\n",
            (unsigned)arenaAddr, (unsigned)ctrl->ringBase,
            (unsigned)(uintptr_t)activeArm.mods,
            (unsigned)(uintptr_t)activeArm.slots,
            (unsigned)slotCap, (unsigned)modCount, (unsigned)requestedSlots);
    return 0;
}

// collector cookie threaded through forEachStubSlot during appendHookMod.
// tracks per-mod drops so we know whether truncation hit this module.
typedef struct {
    uint32_t modDropped;
} HookAppendCookie;

static int appendOneHookSlot(const char *libname, uint32_t nid,
                             uint32_t slotAddr, uint32_t origValue, void *userData)
{
    (void)libname;   // host resolves names via nid-dump; we don't store them on PS3
    // skip unresolved imports: the loader leaves funcTable[i] pointing at
    // a null/sentinel OPD when a declared dependency couldn't be bound at
    // load time. reading loaderOpd[0..1] from such a slot kills the bridge.
    if (origValue == 0) return 1;
    if (activeArm.slotCursor >= activeArm.slotCap) {
        // arena full - count the miss and keep walking so we get an
        // accurate slotsDropped total for the truncation report.
        HookAppendCookie *cookie = (HookAppendCookie *)userData;
        cookie->modDropped++;
        activeArm.slotsDropped++;
        return 1;
    }
    HookSlot *slot = &activeArm.slots[activeArm.slotCursor++];
    slot->nid       = nid;
    slot->slotAddr  = slotAddr;
    slot->origValue = origValue;
    return 1;
}

// append one module's import slots into the active arm. populates a
// HookMod row plus its contiguous slot range. does NOT publish - the
// caller drives publishHookArm once after all mods are appended so the
// trap window across {root, deps...} is a single barrier + tight store
// loop. rc<0:
//   -1 not enough room in activeArm.mods[]
//   -2 libstub out of segments
static inline int appendHookMod(int32_t id, const char *name,
                                const PrxSegment *segs, uint32_t segCount,
                                const PrxLinkage *linkage)
{
    if (activeArm.modCursor >= activeArm.modCap) return -1;
    if (!addrInSegments(linkage->libstubAddr, linkage->libstubSize, segs, segCount)) return -2;

    HookMod *mod = &activeArm.mods[activeArm.modCursor++];
    mod->first = activeArm.slotCursor;
    mod->id    = id;
    uint32_t i = 0;
    while (i + 1 < PRX_NAME_MAX && name[i]) { mod->name[i] = name[i]; i++; }
    mod->name[i] = '\0';

    HookAppendCookie cookie = { 0 };
    forEachStubSlot(linkage->libstubAddr, linkage->libstubSize,
                    segs, segCount, appendOneHookSlot, &cookie);

    mod->count = activeArm.slotCursor - mod->first;
    if (cookie.modDropped > 0) {
        logWarn("[hook] append %s slots=%u dropped=%u (arena full)\n",
                mod->name, (unsigned)mod->count, (unsigned)cookie.modDropped);
    } else {
        logInfo("[hook] append %s slots=%u\n", mod->name, (unsigned)mod->count);
    }
    return 0;
}

// finalise every slot's context + fake OPD, then publish all funcTable
// entries back-to-back. caller must have appended every mod first.
// shrinks the live window where some slots are trampolined and others
// aren't from O(N*setup) to O(N*store).
static inline void publishHookArm(void)
{
    uint32_t trampEntry = (uint32_t)(uintptr_t)hookTrampolineCode;
    for (uint32_t s = 0; s < activeArm.slotCursor; s++) {
        HookSlot       *slot      = &activeArm.slots[s];
        const uint32_t *loaderOpd = (const uint32_t *)(uintptr_t)slot->origValue;
        slot->ctx.origEntry      = loaderOpd[0];
        slot->ctx.origToc        = loaderOpd[1];
        slot->ctx.eventTag       = slot->slotAddr;   // manifest maps slotAddr -> (mod, nid)
        slot->fakeOpd.trampEntry = trampEntry;
        slot->fakeOpd.ctxAddr    = (uint32_t)(uintptr_t)&slot->ctx;
    }
    __sync_synchronize();
    for (uint32_t s = 0; s < activeArm.slotCursor; s++) {
        HookSlot *slot = &activeArm.slots[s];
        *(volatile uint32_t *)(uintptr_t)slot->slotAddr =
            (uint32_t)(uintptr_t)&slot->fakeOpd;
    }
    __sync_synchronize();
    logInfo("[hook] published mods=%u slots=%u tramp=0x%x dropped=%u\n",
            (unsigned)activeArm.modCursor, (unsigned)activeArm.slotCursor,
            (unsigned)trampEntry, (unsigned)activeArm.slotsDropped);
}

// restore every funcTable entry. producer stops feeding the ring as
// soon as this returns. arena is NOT freed - caller drains any remaining
// events then calls freeHookArm.
//
// safety: a target module can be unloaded by VSH while armed (its
// libstub page goes away). a blind volatile store into a freed page
// faults inside lv2 and silently kills the bridge. before touching
// any of a module's slots we re-query its segments via prxInfo; if
// that fails the module is gone, skip the whole range. otherwise each
// slotAddr is bounds-checked against the (possibly-new) segments and
// only restored when still in range.
static inline void restoreHookSlots(void)
{
    if (!isHookArmed()) return;
    logInfo("[hook] restore begin: mods=%u slots=%u arena=0x%x\n",
            (unsigned)activeArm.modCursor, (unsigned)activeArm.slotCursor,
            (unsigned)activeArm.arenaAddr);

    PrxSegment segs[PRX_SEGMENTS_MAX];
    char       name[PRX_NAME_MAX];
    char       file[PRX_FILENAME_MAX];

    for (uint32_t m = 0; m < activeArm.modCursor; m++) {
        const HookMod *mod = &activeArm.mods[m];
        uint32_t segCount = 0;
        int32_t  rc = prxInfo(mod->id, name, file, segs, PRX_SEGMENTS_MAX, &segCount);
        if (rc < 0 || segCount == 0) {
            logWarn("[hook] restore mod[%u]=%s gone (rc=0x%x), skipping %u slots\n",
                    (unsigned)m, mod->name, (unsigned)rc, (unsigned)mod->count);
            continue;
        }
        if (segCount > PRX_SEGMENTS_MAX) segCount = PRX_SEGMENTS_MAX;
        uint32_t restored = 0, skipped = 0;
        uint32_t end = mod->first + mod->count;
        for (uint32_t s = mod->first; s < end; s++) {
            HookSlot *slot = &activeArm.slots[s];
            if (!addrInSegments(slot->slotAddr, 4, segs, segCount)) {
                skipped++;
                continue;
            }
            __sync_synchronize();
            *(volatile uint32_t *)(uintptr_t)slot->slotAddr = slot->origValue;
            __sync_synchronize();
            restored++;
        }
        if (skipped) {
            logWarn("[hook] restore mod[%u]=%s restored=%u skipped=%u\n",
                    (unsigned)m, mod->name, (unsigned)restored, (unsigned)skipped);
        } else {
            logInfo("[hook] restore mod[%u]=%s restored=%u\n",
                    (unsigned)m, mod->name, (unsigned)restored);
        }
    }
    logInfo("[hook] restored mods=%u slots=%u\n",
            (unsigned)activeArm.modCursor, (unsigned)activeArm.slotCursor);
}

// free arena, zero state. caller must have called restoreHookSlots
// first and drained any final events.
static inline void freeHookArm(void)
{
    if (activeArm.arenaAddr != 0) sysMemFree(activeArm.arenaAddr);
    hookRingCtrl = 0;
    activeArm.arenaAddr      = 0;
    activeArm.mods           = 0;
    activeArm.slots          = 0;
    activeArm.slotCap        = 0;
    activeArm.modCap         = 0;
    activeArm.slotCursor     = 0;
    activeArm.modCursor      = 0;
    activeArm.slotsRequested = 0;
    activeArm.slotsDropped   = 0;
}

// drain newly-recorded events from the ring. consumer-side: bumps
// readIdx in the ring control header, hands one contiguous (or two
// when wrapping) span of 16-byte events to `sink`. `count` is the
// number of EVENTS (not words). returns the count of events drained
// this call. ring overruns are reported via hookDropCount().
typedef int (*HookSpanSink)(void *cookie, const uint32_t *events, uint32_t count);

static inline uint32_t drainHookEvents(HookSpanSink sink, void *cookie)
{
    if (hookRingCtrl == 0) return 0;
    HookRingCtrl   *ctrl = hookRingCtrl;
    const uint32_t *ring = (const uint32_t *)(uintptr_t)ctrl->ringBase;
    uint32_t write = ctrl->writeIdx;
    uint32_t read  = ctrl->readIdx;
    uint32_t avail = write - read;
    if (avail == 0) return 0;
    if (avail > HOOK_RING_SIZE) {
        read  = write - HOOK_RING_SIZE;
        avail = HOOK_RING_SIZE;
    }
    uint32_t start = read & HOOK_RING_MASK;
    uint32_t first = HOOK_RING_SIZE - start;
    if (first > avail) first = avail;
    // each event is HOOK_EVENT_WORDS words; sink takes event count and
    // gets the base word pointer for the contiguous span.
    if (sink(cookie, &ring[start * HOOK_EVENT_WORDS], first) < 0) return 0;
    if (avail > first) {
        if (sink(cookie, &ring[0], avail - first) < 0) {
            ctrl->readIdx = read + first;
            return first;
        }
    }
    ctrl->readIdx = write;
    return avail;
}

// drop count = events that were overwritten before the drain caught up.
static inline uint32_t hookDropCount(void)
{
    if (hookRingCtrl == 0) return 0;
    uint32_t write = hookRingCtrl->writeIdx;
    uint32_t read  = hookRingCtrl->readIdx;
    uint32_t gap   = write - read;
    return (gap > HOOK_RING_SIZE) ? (gap - HOOK_RING_SIZE) : 0;
}
