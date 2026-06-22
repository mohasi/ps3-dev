#pragma once

// module-trace orchestration: arm/disarm an imports trampoline on a
// loaded prx, drain the event ring to a self-describing capture file
// under /dev_hdd0/tmp/, then write a manifest on disarm so the host
// can map each event back to (module, nid).
//
// file layout (single file, no separate manifest):
//   offset  0: 16-byte header
//     [0..4)   magic  "TRAC"
//     [4..8)   version u32 (big-endian) = 3
//     [8..12)  manifestOffset u32 (patched on close; 0 means "still open")
//     [12..16) reserved 0
//   offset 16: event stream (each event = 16 bytes big-endian:
//              { slotAddr, r3, r4, r5 } captured at call site)
//   manifestOffset: ascii section, starts with "\n==MANIFEST==\n",
//                   then one line per slot: "slot 0x<addr>\t<module>\t0x<nid>\n",
//                   then "==END==\n". offsets back-resolve every event to
//                   (module, nid) for the analyzer; r3..r5 give us
//                   `this` + first two args (callback addrs, handles, etc).

#include <sys/ppu_thread.h>

#include "cmd-common.h"
#include "thread.h"
#include "module-hook.h"
#include "vfs.h"

#define HOOK_CAPTURE_DIR  "/dev_hdd0/tmp"
#define HOOK_CAPTURE_PATH HOOK_CAPTURE_DIR "/trace-capture.bin"
#define HOOK_HEADER_BYTES 16
#define HOOK_FORMAT_VER   3u

static volatile int     hookDrainRunning = 0;
static volatile int     hookDrainStop    = 0;
static sys_ppu_thread_t hookDrainTid     = 0;
static VfsFile          hookDrainFile;         // capture file (valid while hookDrainOpen)
static volatile int     hookDrainOpen    = 0;  // VfsFile has no -1 sentinel; track openness
static uint32_t         hookDrainWritten = 0;  // events safely on disk

// arm staging: per-module record we hold while resolving root + deps.
// these are file-scope statics (not stack locals) because a single
// ArmTarget is ~720 bytes and the connection thread that runs
// cmdModuleTraceOn only has a 16KB stack - 32 * 720 = ~22KB would
// blow it silently. cmd dispatch is serialised (single host policy)
// so one static table is safe.
#define ARM_TARGETS_MAX 32

typedef struct {
   int32_t    id;
   char       name[PRX_NAME_MAX];
   PrxSegment segs[PRX_SEGMENTS_MAX];
   uint32_t   segCount;
   PrxLinkage linkage;
   uint32_t   slotCount;   // populated by count pass
} ArmTarget;

// prxInfo requires a real caller-owned filename buffer (PRX_FILENAME_MAX
// = 512 bytes), but we never use the path. one shared scratch keeps
// ArmTarget itself small (otherwise file[512] * 32 targets blows the
// 16KB connection-thread stack on its own).
static char armFileScratch[PRX_FILENAME_MAX];

// pass-1 visitor: count patchable slots in one module (skips unresolved
// imports, matching appendOneHookSlot's filter).
static int countOneArmSlot(const char *libname, uint32_t nid,
                           uint32_t slotAddr, uint32_t origValue, void *userData)
{
   (void)libname; (void)nid; (void)slotAddr;
   if (origValue == 0) return 1;
   (*(uint32_t *)userData)++;
   return 1;
}

// load name + segments + linkage for a prx id. returns 0/-1 (-1 on any
// syscall failure; caller treats as "skip this target").
static int loadArmTarget(int32_t id, ArmTarget *t)
{
   t->id       = id;
   t->segCount = 0;
   t->slotCount = 0;
   for (uint32_t i = 0; i < PRX_NAME_MAX; i++) t->name[i] = 0;
   for (uint32_t i = 0; i < sizeof t->linkage; i++) ((char *)&t->linkage)[i] = 0;
   int32_t rc = prxInfo(id, t->name, armFileScratch, t->segs, PRX_SEGMENTS_MAX, &t->segCount);
   if (rc < 0) { logWarn("[sdb] loadArmTarget id=%d prxInfo rc=0x%x\n", (int)id, (unsigned)rc); return -1; }
   rc = prxLinkage(id, &t->linkage, NULL, 0, NULL);
   if (rc < 0) { logWarn("[sdb] loadArmTarget id=%d prxLinkage rc=0x%x\n", (int)id, (unsigned)rc); return -1; }
   return 0;
}

// dep resolution: root imports lib X; for every other loaded prx, ask
// whether it exports lib X. one level - if a matched dep itself imports
// lib Y not exported by anyone in our set, calls through Y stay
// invisible. acceptable: the gap shows up in the capture and the
// operator promotes that module manually next run.

typedef struct {
   char     libs[ARM_TARGETS_MAX * 4][INSPECT_LIBNAME_MAX];
   uint32_t count;
} ImportLibSet;

// shared staging buffers - see ARM_TARGETS_MAX comment above. only ever
// touched on the connection thread under serialised host dispatch.
static ArmTarget    armTargets[ARM_TARGETS_MAX];
static ArmTarget    armScratch;
static ImportLibSet armWanted;

static int collectImportLib(const char *libname, uint32_t nfunc, void *userData)
{
   (void)nfunc;
   if (!libname || !libname[0]) return 1;
   ImportLibSet *set = (ImportLibSet *)userData;
   if (set->count >= sizeof set->libs / sizeof set->libs[0]) return 1;
   for (uint32_t i = 0; i < set->count; i++) {
      if (strEq(set->libs[i], libname)) return 1;
   }
   uint32_t i = 0;
   while (i + 1 < INSPECT_LIBNAME_MAX && libname[i]) { set->libs[set->count][i] = libname[i]; i++; }
   set->libs[set->count][i] = '\0';
   set->count++;
   return 1;
}

typedef struct {
   const ImportLibSet *want;
   int                 matched;
} ExportMatchProbe;

static int probeExportMatch(const char *libname, uint32_t nfunc, void *userData)
{
   (void)nfunc;
   if (!libname || !libname[0]) return 1;
   ExportMatchProbe *p = (ExportMatchProbe *)userData;
   for (uint32_t i = 0; i < p->want->count; i++) {
      if (strEq(p->want->libs[i], libname)) { p->matched = 1; return 0; }
   }
   return 1;
}

// fill `set` with every distinct import-lib name declared by root.
static void collectRootImports(const ArmTarget *root, ImportLibSet *set)
{
   set->count = 0;
   forEachStubLib(root->linkage.libstubAddr, root->linkage.libstubSize,
                  root->segs, root->segCount, collectImportLib, set);
}

// return 1 if this prx exports any lib in `set`.
static int prxExportsAny(int32_t id, const ImportLibSet *set, ArmTarget *scratch)
{
   if (loadArmTarget(id, scratch) < 0) return 0;
   if (!addrInSegments(scratch->linkage.libentAddr, scratch->linkage.libentSize,
                       scratch->segs, scratch->segCount)) {
      logInfo("[sdb] dep id=%d name=%s libent out-of-seg (libent=0x%x size=%u)\n",
              (int)id, scratch->name,
              (unsigned)scratch->linkage.libentAddr,
              (unsigned)scratch->linkage.libentSize);
      return 0;
   }
   ExportMatchProbe probe = { set, 0 };
   forEachEntLib(scratch->linkage.libentAddr, scratch->linkage.libentSize,
                 scratch->segs, scratch->segCount, probeExportMatch, &probe);
   return probe.matched;
}

// resolve deps for `root` into `targets[1..]`. returns the number of
// deps added (0 on success-with-no-deps, may be 0 if cap reached).
// uses file-scope armWanted / armScratch (stack-sensitive - see top).
static uint32_t stageRootDeps(const ArmTarget *root, ArmTarget *targets, uint32_t *count)
{
   collectRootImports(root, &armWanted);
   logInfo("[sdb] trace-on deps: root imports %u libs\n", (unsigned)armWanted.count);
   if (armWanted.count == 0) return 0;

   uint32_t ids[MODULE_IDS_MAX];
   uint32_t total = 0;
   if (prxList(ids, MODULE_IDS_MAX, &total) < 0) return 0;
   logInfo("[sdb] trace-on deps: scanning %u prx ids\n", (unsigned)total);

   uint32_t added = 0;
   for (uint32_t i = 0; i < total && *count < ARM_TARGETS_MAX; i++) {
      int32_t id = (int32_t)ids[i];
      if (id == root->id) continue;
      // yield between per-id prxInfo/prxLinkage bursts. same kernel
      // state hazard as findModuleByName: without a scheduling gap
      // the follow-up arming syscalls observe inconsistent loader
      // state and wedge. previously a per-iteration logInfo here
      // was incidentally providing this yield.
      yieldThread();
      int matched = prxExportsAny(id, &armWanted, &armScratch);
      if (!matched) continue;
      // armScratch already has loaded data; copy byte-wise into the
      // targets slot (no libc memcpy in the bridge).
      ArmTarget *dst = &targets[*count];
      const char *src = (const char *)&armScratch;
      char       *out = (char *)dst;
      for (uint32_t b = 0; b < sizeof(ArmTarget); b++) out[b] = src[b];
      (*count)++;
      added++;
   }
   return added;
}

// span sink for drainHookEvents: append raw event records to the open
// capture file. each event is HOOK_EVENT_BYTES (16) bytes: slotAddr +
// r3 + r4 + r5. count is event count, not word count.
static int hookFileSink(void *cookie, const uint32_t *events, uint32_t count)
{
   (void)cookie;
   if (!hookDrainOpen) return -1;
   if (writeFs(&hookDrainFile, events, (uint64_t)count * HOOK_EVENT_BYTES) < 0) return -1;
   hookDrainWritten += count;
   return 0;
}

// background drain loop: while armed, copy any new ring events to disk.
// runs at the standard vsh-plugin priority; small sleep so we don't burn
// the cpu when the producer is idle. on stop signal, drain one last time
// to flush whatever the producer wrote between the last tick and stop.
static void hookDrainThread(uint64_t arg)
{
   (void)arg;
   hookDrainRunning = 1;
   while (!hookDrainStop) {
      drainHookEvents(hookFileSink, NULL);
      sleepMs(20);
   }
   drainHookEvents(hookFileSink, NULL);   // final flush
   hookDrainRunning = 0;
   exitThread();
}

static int startHookDrain(void)
{
   if (openFs(HOOK_CAPTURE_PATH,
              VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC,
              &hookDrainFile) != 0) {
      hookDrainOpen = 0;
      return -1;
   }
   hookDrainOpen = 1;
   // write 16-byte placeholder header. manifestOffset stays 0 until the
   // disarm path patches it in. version is fixed; reserved is zero.
   uint8_t header[HOOK_HEADER_BYTES] = {
      'T','R','A','C',
      0,0,0,(uint8_t)HOOK_FORMAT_VER,   // version big-endian u32
      0,0,0,0,                          // manifestOffset (patched on close)
      0,0,0,0                           // reserved
   };
   if (writeFs(&hookDrainFile, header, HOOK_HEADER_BYTES) < 0) {
      closeFs(&hookDrainFile);
      hookDrainOpen = 0;
      return -1;
   }
   hookDrainWritten = 0;
   hookDrainStop    = 0;
   int rc = spawnJoinableThread(&hookDrainTid, hookDrainThread, 0,
                                THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "bridge-hook-drain");
   if (rc != 0) {
      closeFs(&hookDrainFile);
      hookDrainOpen = 0;
      return -1;
   }
   return 0;
}

// append the manifest at end-of-events and patch manifestOffset in the
// header so the reader can find it. one row per slot. a trailing
// "==SUMMARY==" block records truncation + ring-drop counters so the
// analyzer can flag captures where the arena overflowed or the drain
// fell behind, even without the live OK reply.
static void writeHookManifest(VfsFile *f)
{
   int64_t pos = seekFs(f, 0, VFS_SEEK_CUR);
   if (pos < 0) return;
   uint32_t manifestOff = (uint32_t)pos;

   static const char sentinel[] = "\n==MANIFEST==\n";
   writeFs(f, sentinel, sizeof sentinel - 1);

   char line[160];
   for (uint32_t m = 0; m < activeArm.modCursor; m++) {
      const HookMod *mod = &activeArm.mods[m];
      for (uint32_t i = 0; i < mod->count; i++) {
         const HookSlot *slot = &activeArm.slots[mod->first + i];
         int n = snprintf(line, sizeof line, "slot 0x%08x\t%s\t0x%08x\n",
                          (unsigned)slot->slotAddr, mod->name, (unsigned)slot->nid);
         if (n > 0) writeFs(f, line, (uint64_t)n);
      }
   }
   static const char sumHead[] = "==SUMMARY==\n";
   writeFs(f, sumHead, sizeof sumHead - 1);
   int sn = snprintf(line, sizeof line,
                     "slots\trequested=%u armed=%u dropped=%u\n"
                     "events\twritten=%u ring_dropped=%u\n",
                     (unsigned)getHookSlotsRequested(),
                     (unsigned)getHookSlotsArmed(),
                     (unsigned)getHookSlotsDropped(),
                     (unsigned)hookDrainWritten,
                     (unsigned)getHookDropCount());
   if (sn > 0) writeFs(f, line, (uint64_t)sn);
   static const char tail[] = "==END==\n";
   writeFs(f, tail, sizeof tail - 1);

   // patch manifestOffset (big-endian u32) at byte 8 of the header.
   uint8_t be[4] = {
      (uint8_t)(manifestOff >> 24),
      (uint8_t)(manifestOff >> 16),
      (uint8_t)(manifestOff >> 8),
      (uint8_t)(manifestOff)
   };
   if (seekFs(f, 8, VFS_SEEK_SET) >= 0) {
      writeFs(f, be, 4);
   }
}

// module-trace-on <root> [withDeps]
//   enumerate root's import slots and patch them with the global
//   trampoline. with `withDeps`, also arm every loaded prx whose
//   exports satisfy any of root's imports (one level), so calls
//   *out of* the deps are recorded in the same timeline. all
//   modules are armed atomically in one publish loop.
//   reply payload lists each armed module's slot count.
static void cmdModuleTraceOn(int cli, const char *args)
{
   if (!args || !args[0]) {
      sendReply(cli, SDB_ERR, "usage: module-trace-on <root> [withDeps]"); return;
   }
   // parse <root> [withDeps]
   char rootName[PRX_NAME_MAX];
   uint32_t i = 0;
   while (args[i] && args[i] != ' ' && i + 1 < PRX_NAME_MAX) { rootName[i] = args[i]; i++; }
   rootName[i] = '\0';
   const char *tail = args + i;
   while (*tail == ' ') tail++;
   int withDeps = (*tail && strEq(tail, "withDeps"));
   if (*tail && !withDeps) {
      sendReply(cli, SDB_ERR, "usage: module-trace-on <root> [withDeps]"); return;
   }

   int32_t rootId = findModuleByName(rootName);
   if (rootId < 0) { sendReply(cli, SDB_ERR, "module not found"); return; }

   // file-scope staging table (see ARM_TARGETS_MAX). discarded once
   // allocHookArm + appendHookMod loop have copied everything into
   // activeArm.
   ArmTarget *targets = armTargets;
   uint32_t   targetCount = 0;
   logInfo("[sdb] trace-on '%s' withDeps=%d rootId=%d\n", rootName, withDeps, (int)rootId);
   if (loadArmTarget(rootId, &targets[0]) < 0) {
      sendReply(cli, SDB_ERR, "prxInfo/prxLinkage failed"); return;
   }
   targetCount = 1;
   logInfo("[sdb] trace-on root loaded: name=%s segs=%u libstub=0x%x size=%u\n",
           targets[0].name, (unsigned)targets[0].segCount,
           (unsigned)targets[0].linkage.libstubAddr,
           (unsigned)targets[0].linkage.libstubSize);

   if (withDeps) {
      uint32_t depsAdded = stageRootDeps(&targets[0], targets, &targetCount);
      logInfo("[sdb] trace-on %s withDeps -> %u deps (total targets=%u)\n",
              rootName, (unsigned)depsAdded, (unsigned)targetCount);
   }

   // pass 1: count slots across all targets. skips unresolved imports.
   uint32_t totalSlots = 0;
   for (uint32_t t = 0; t < targetCount; t++) {
      if (!addrInSegments(targets[t].linkage.libstubAddr, targets[t].linkage.libstubSize,
                          targets[t].segs, targets[t].segCount)) {
         logWarn("[sdb] count: %s libstub out-of-seg (0x%x +%u) - skipping\n",
                 targets[t].name,
                 (unsigned)targets[t].linkage.libstubAddr,
                 (unsigned)targets[t].linkage.libstubSize);
         targets[t].slotCount = 0;
         continue;
      }
      uint32_t n = 0;
      forEachStubSlot(targets[t].linkage.libstubAddr, targets[t].linkage.libstubSize,
                      targets[t].segs, targets[t].segCount, countOneArmSlot, &n);
      targets[t].slotCount = n;
      totalSlots += n;
   }
   logInfo("[sdb] trace-on count pass: targets=%u totalSlots=%u\n",
           (unsigned)targetCount, (unsigned)totalSlots);
   if (totalSlots == 0) {
      sendReply(cli, SDB_ERR, "no patchable import slots"); return;
   }

   int rc = allocHookArm(totalSlots, targetCount);
   if (rc < 0) {
      const char *msg = (rc == -1) ? "arena alloc failed"
                  : (rc == -3) ? "already armed: run module-trace-off first"
                  : "allocHookArm failed";
      sendReply(cli, SDB_ERR, msg); return;
   }

   // pass 2: append every target's slots into the active arm.
   for (uint32_t t = 0; t < targetCount; t++) {
      if (targets[t].slotCount == 0) continue;
      if (appendHookMod(targets[t].id, targets[t].name, targets[t].segs, targets[t].segCount,
                        &targets[t].linkage) < 0) {
         // shouldn't happen: cap == targetCount, libstub already validated.
         logError("[sdb] appendHookMod failed for %s\n", targets[t].name);
         restoreHookSlots();
         freeHookArm();
         sendReply(cli, SDB_ERR, "appendHookMod failed");
         return;
      }
   }
   logInfo("[sdb] publish: mods=%u slots=%u\n",
           (unsigned)activeArm.modCursor, (unsigned)activeArm.slotCursor);
   publishHookArm();
   logInfo("[sdb] publish done\n");

   if (startHookDrain() < 0) {
      logError("[sdb] startHookDrain failed\n");
      // publish succeeded but no log destination - back out the arm.
      restoreHookSlots();
      freeHookArm();
      sendReply(cli, SDB_ERR, "cannot open " HOOK_CAPTURE_PATH);
      return;
   }
   logInfo("[sdb] drain thread started\n");

   // reply: one hmod row per armed module, an htrunc row if the arena
   // couldn't hold every slot, then the totals.
   uint32_t off = 0;
   for (uint32_t m = 0; m < activeArm.modCursor && off + 96 < REPLY_BUF_BYTES; m++) {
      const HookMod *mod = &activeArm.mods[m];
      off += (uint32_t)snprintf(replyBuf + off, REPLY_BUF_BYTES - off,
                                "hmod\t%s\tslots=%u\n",
                                mod->name, (unsigned)mod->count);
   }
   if (getHookSlotsDropped() > 0) {
      off += (uint32_t)snprintf(replyBuf + off, REPLY_BUF_BYTES - off,
                                "htrunc\trequested=%u armed=%u dropped=%u\n",
                                (unsigned)getHookSlotsRequested(),
                                (unsigned)getHookSlotsArmed(),
                                (unsigned)getHookSlotsDropped());
   }
   off += (uint32_t)snprintf(replyBuf + off, REPLY_BUF_BYTES - off,
                             "hsum\tmods=%u\tslots=%u\n",
                             (unsigned)activeArm.modCursor, (unsigned)activeArm.slotCursor);
   if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
   if (off) sendBytes(cli, replyBuf, (int)off);
}

// module-trace-off
//   restore every armed slot, flush + close the capture file with its
//   manifest, free heap. no name argument - one arm at a time, disarm
//   is unconditional.
//   reply: "hevt\ttotal=<n> dropped=<n> file=<path>\n"
static void cmdModuleTraceOff(int cli, const char *args)
{
   (void)args;
   if (!isHookArmed()) { sendReply(cli, SDB_ERR, "not traced"); return; }

   // restore funcTable first so the producer stops adding to the ring,
   // then stop the drain thread (its final pass flushes whatever was
   // already queued). manifest needs activeArm contents, so write it
   // before freeHookArm clears them.
   restoreHookSlots();
   hookDrainStop = 1;
   if (hookDrainTid != 0) {
      joinThread(hookDrainTid);
      hookDrainTid = 0;
   }
   if (hookDrainOpen) {
      writeHookManifest(&hookDrainFile);
      closeFs(&hookDrainFile);
      hookDrainOpen = 0;
   }

   uint32_t dropped = getHookDropCount();
   uint32_t total   = hookDrainWritten + dropped;
   char     reply[160];
   int      n = snprintf(reply, sizeof reply,
                         "hevt\ttotal=%u dropped=%u file=%s\n",
                         (unsigned)total, (unsigned)dropped, HOOK_CAPTURE_PATH);
   if (sendFrameHeader(cli, SDB_OK, (uint32_t)n) >= 0) sendBytes(cli, reply, n);

   freeHookArm();
}
