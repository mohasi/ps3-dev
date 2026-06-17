#pragma once

// read-only introspection commands: module-list, process-list,
// module-info, process-info. each handler formats a tab-separated
// text payload into the shared replyBuf and frames it as one OK.

#include "cmd-common.h"
#include "module-inspect.h"

// list every prx loaded into vsh.self. payload body is one
// "<name>\t<file>\n" record per module (final newline included),
// returned as a single OK frame so the host reads it all in one shot.
// the runtime prx id is omitted - the host addresses modules by name.
static void cmdModuleList(int cli)
{
   char     name[PRX_NAME_MAX];
   char     file[PRX_FILENAME_MAX];
   uint32_t count;

   int32_t rc = prxList(moduleIds, MODULE_IDS_MAX, &count);
   if (rc < 0) {
      sendErrRc(cli, "prxList", rc);
      return;
   }

   uint32_t off = 0;
   for (uint32_t i = 0; i < count; i++) {
      if (prxName((int32_t)moduleIds[i], name, file) < 0) continue;
      off += (uint32_t)snprintf(replyBuf + off, REPLY_BUF_BYTES - off,
                                "%s\t%s\n", name, file);
   }
   if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
   if (off) sendBytes(cli, replyBuf, (int)off);
}

// enumerate processes the bridge can introspect. on cex the dbg syscalls
// for cross-process enumeration (sc908 / sc492 / sc493) are either ENOSYS
// or locked to the calling pid - see git history for the read-only probe
// that established this. the bridge runs inside vsh, so the only process
// we can actually introspect is vsh itself. we still emit a tabular
// payload so the client can fan out if a future cfw escalation (dex /
// rebug / qa flags) unlocks more.
// payload: one "<name>\tlive\n" record per process.
static void cmdProcessList(int cli)
{
   static const char payload[] = "vsh\tlive\n";
   uint32_t off = sizeof payload - 1;
   if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
   sendBytes(cli, payload, (int)off);
}

// dump identity, segments, linkage tables, exports and imports of one
// module. text payload, one record per line:
//   id\t<id>\n
//   name\t<name>\n
//   file\t<filename>\n
//   seg\t<index>\t<type>\t<base>\t<filesz>\t<memsz>\n   (repeated)
//   libent\t<addr>\t<size>\n
//   libstub\t<addr>\t<size>\n
//   ent\t<libname>\tnfunc=<n>\tnvar=<n>\tntls=<n>\n
//     ef\t<nid>\t<opd>\n              (repeated, one per export function)
//   stub\t<libname>\tnfunc=<n>\tnvar=<n>\tntls=<n>\n
//     sf\t<nid>\t<stubOpd>\n          (repeated, one per import function)
// every pointer the walker dereferences is bounds-checked against the
// module's segments first - a bad VA inside a record is skipped rather
// than crashing the VSH process.
static void cmdModuleInfo(int cli, const char *args)
{
   if (!args || !args[0]) {
      sendReply(cli, SDB_ERR, "usage: module-info <name>");
      return;
   }
   int32_t id = findModuleByName(args);
   if (id < 0) { sendReply(cli, SDB_ERR, "module not found"); return; }

   char       name[PRX_NAME_MAX];
   char       file[PRX_FILENAME_MAX];
   PrxSegment segs[PRX_SEGMENTS_MAX];
   uint32_t   segCount = 0;
   int32_t rc = prxInfo(id, name, file, segs, PRX_SEGMENTS_MAX, &segCount);
   if (rc < 0) {
      sendErrRc(cli, "prxInfo", rc);
      return;
   }
   if (segCount > PRX_SEGMENTS_MAX) segCount = PRX_SEGMENTS_MAX;

   PrxLinkage link = {0};
   int32_t lrc = prxLinkage(id, &link, NULL, 0, NULL);
   int linkageOk = (lrc >= 0);
   int libentOk  = linkageOk && addrInSegments(link.libentAddr,  link.libentSize,  segs, segCount);
   int libstubOk = linkageOk && addrInSegments(link.libstubAddr, link.libstubSize, segs, segCount);

   uint32_t off = (uint32_t)snprintf(replyBuf, REPLY_BUF_BYTES,
                                     "id\t%u\nname\t%s\nfile\t%s\n",
                                     (unsigned)id, name, file);
   off = emitSegments(replyBuf, off, REPLY_BUF_BYTES, segs, segCount, "seg");
   if (linkageOk) {
      off += (uint32_t)snprintf(replyBuf + off, REPLY_BUF_BYTES - off,
                                "libent\t0x%x\t%u\nlibstub\t0x%x\t%u\n",
                                (unsigned)link.libentAddr, (unsigned)link.libentSize,
                                (unsigned)link.libstubAddr, (unsigned)link.libstubSize);
   }
   if (libentOk) {
      off = emitLibEnts(replyBuf, off, REPLY_BUF_BYTES,
                        link.libentAddr, link.libentSize,
                        segs, segCount, "ent", "ef", "ev");
   }
   if (libstubOk) {
      off = emitLibStubs(replyBuf, off, REPLY_BUF_BYTES,
                         link.libstubAddr, link.libstubSize,
                         segs, segCount, "stub", "sf", "sv");
   }

   if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
   if (off) sendBytes(cli, replyBuf, (int)off);
}

// expand a single process. text payload:
//   pid    0x<hex>
//   name   <text>
//   sdk    0x<hex>      (from sys_process_get_sdk_version, self)
//   mod    <id>\t<name>\t<filename>     (one row per loaded prx)
// followed by the main executable's own segments + imports/exports
// (the main self is not a prx and so doesn't appear in mod rows):
//   pseg   <i>\t<type>\t<base>\t<filesz>\t<memsz>
//   pstub  <libname>\tnfunc=<n>\tnvar=<n>\tntls=<n>
//     psf  <nid>\t<addr>                (repeated, one per import func)
//   pent   <libname>\tnfunc=<n>\tnvar=<n>\tntls=<n>
//     pef  <nid>\t<addr>                (repeated, one per export func)
static void cmdProcessInfo(int cli, const char *args)
{
   if (!args || !args[0] || !strEq(args, "vsh")) {
      sendReply(cli, SDB_ERR, "usage: process-info vsh");
      return;
   }

   char     name[PRX_NAME_MAX];
   char     file[PRX_FILENAME_MAX];
   uint32_t count = 0;
   int32_t  rc = prxList(moduleIds, MODULE_IDS_MAX, &count);
   if (rc < 0) {
      sendErrRc(cli, "prxList", rc);
      return;
   }

   int64_t  pid    = scCall1(1, 0);                                    // sys_process_getpid
   uint32_t sdkVer = 0;
   scCall2(25, (uint64_t)pid, (uint64_t)(uintptr_t)&sdkVer);           // sys_process_get_sdk_version

   // shared scratch is sized for the biggest payload: vsh.self exports
   // are ~4400 functions (~120 KiB of "pef" rows). pid/sdk + ~128 mod
   // rows + main-exe segs/libstub/libent all fit comfortably.
   uint32_t off = (uint32_t)snprintf(replyBuf, REPLY_BUF_BYTES,
                                     "pid\t0x%llx\nname\tvsh\nsdk\t0x%x\n",
                                     (unsigned long long)pid, (unsigned)sdkVer);
   for (uint32_t i = 0; i < count; i++) {
      if (prxName((int32_t)moduleIds[i], name, file) < 0) continue;
      off += (uint32_t)snprintf(replyBuf + off, REPLY_BUF_BYTES - off,
                                "mod\t%s\t%s\n", name, file);
   }

   // main-exe linkage: parse the ELF mapped at 0x10000 (the conventional
   // main-self load base; confirmed live via prior probes) and emit
   // pseg / pstub / psf / pent / pef rows. tag prefix is "p" so the
   // wire format stays generic for any future process the bridge can
   // see, not vsh-specific.
   const volatile uint8_t *elf = (const volatile uint8_t *)(uintptr_t)0x10000;
   PrxSegment psegs[8];
   uint32_t   pSegCount = loadElfSegments(elf, psegs, 8);
   off = emitSegments(replyBuf, off, REPLY_BUF_BYTES, psegs, pSegCount, "pseg");
   if (pSegCount > 0) {
      off = scanLinkage(replyBuf, off, REPLY_BUF_BYTES, psegs, pSegCount,
                        "pent", "pef", "pev", "pstub", "psf", "psv");
   }

   if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
   if (off) sendBytes(cli, replyBuf, (int)off);
}
