#pragma once

// tcp accept loop and command dispatch for simple-debug-bridge.
// listens on port 8785. one persistent client at a time: the host opens
// a single tcp socket and reuses it for every command. each request is
// "<cmd>[ args]\n" (with optional raw upload bytes after the newline for
// upload commands like save-file). each reply is the framed format
// "<STATUS> <n>\n[<n bytes>]" — see sendFrame() / sendFrameHeader() in wire.h.

#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/synchronization.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "dbg.h"
#include "thread.h"
#include "vsh.h"
#include "syscall.h"
#include "printf.h"
#include "string-utilities.h"
#include "wire.h"
#include "log-backlog.h"
#include "fileio.h"
#include "plugin.h"
#include "capture.h"
#include "pkg.h"

#define SDB_PORT          8785
#define SDB_BUF_MAX       512
#define SDB_LOG_BODY_MAX  4096
#define SDB_OK            "OK"
#define SDB_ERR           "ERR"

// sys_sm_shutdown modes (psdevwiki) — see syscall.h sysPower()
enum {
    POWER_SHUTDOWN   = 0x1100,
    POWER_REBOOT     = 0x1200,
    POWER_VSH_REBOOT = 0x0200
};

// teardown state — only relevant when something calls sys_prx_stop_module
// on us (hot reload during dev). on power-off / reboot lv2 tears the whole
// system down externally.
static volatile int      isServerRunning  = 0;
static int               serverListenFd = -1;
static int               serverHostFd   = -1; // current connected host, if any
static sys_ppu_thread_t  serverThreadId = 0;

// serializes writes to the host fd. command replies happen on the host
// thread; producer LOG frames are forwarded from producer threads. without
// this lock a producer write can interleave bytes inside a host reply
// payload and desync the host's framed reader.
static sys_lwmutex_t     serverHostWriteMx;

// active producer registry. one app slot (newest wins — if a second app
// registers we drop the previous one) and N plugin slots. capture routing
// will later prefer the app slot when present, but logs flow from any
// registered producer to the host as soon as the host is connected.
#define SDB_MAX_PLUGINS 8
#define SDB_NAME_MAX    32

typedef struct {
    int  fd;
    char name[SDB_NAME_MAX];
} ProducerSlot;

static ProducerSlot appSlot    = { -1, { 0 } };
static ProducerSlot pluginSlots[SDB_MAX_PLUGINS];
static sys_lwmutex_t serverRegistryMx;

// send a full buffer, retrying partial writes. low-level primitive
// underneath sendReply / sendFrame and used by callers that stream a
// known-size payload after sendFrameHeader (captureSendRow, cmdModuleList).
// sendBytes / sendFrameHeader / sendFrame / receiveLine come from wire.h.

// pre-connect ring buffer. producers (and the bridge itself) start logging
// well before the host attaches; without a backlog those lines would be
// silently dropped and the operator would never see plugin startup output
// in the Logs tab. drained on host connect; older lines overwritten if the
// host stays away too long.
static LogBacklog logBacklog;

static inline void pushBacklog(const char *buf, int len)
{
    pushLogBacklog(&logBacklog, buf, len);
}

// drain callback: forward each buffered line as a LOG frame to the host.
static int sendBacklogToHost(const char *data, int len, void *user)
{
    int fd = *(int *)user;
    if (sendFrame(fd, "LOG", data, len) < 0) {
        shutdown(fd, SHUT_RDWR);
        return -1;
    }
    return 0;
}

// drain pre-connect lines in chronological order. caller must hold
// serverHostWriteMx and have serverHostFd set to the just-attached host.
static inline void drainBacklog(int fd)
{
    drainLogBacklog(&logBacklog, sendBacklogToHost, &fd);
}

// forward a LOG line from a producer thread to the host. takes the host
// write mutex so the "LOG <n>\n<bytes>" frame never interleaves with a
// reply to a host command. when no host is connected, queues into the
// ring buffer so the lines are replayed on the next host-connect.
static void forwardLogToHost(const char *buf, int len)
{
    sys_lwmutex_lock(&serverHostWriteMx, 0);
    int fd = serverHostFd;
    if (fd >= 0) {
        if (sendFrame(fd, "LOG", buf, len) < 0) {
            shutdown(fd, SHUT_RDWR);
        }
    } else {
        pushBacklog(buf, len);
    }
    sys_lwmutex_unlock(&serverHostWriteMx);
}

// every reply on the wire is "<status> <n>\n[<n bytes>]" where status is
// OK or ERR and n is the decimal payload length. n=0 is valid ("OK 0\n").
// the host reads the header line then exactly n bytes — no other framing.
//
//   sendReply       — c-string payload, length figured out internally.
//                     covers empty replies (""), errors, and every text OK.
//   sendFrameHeader — write just the header for a known-size payload; caller
//                     follows up with sendBytes / sendFileWindow / captureRegion
//                     to produce the n bytes (capture, get-file, module-list).
static inline int sendReply(int fd, const char *status, const char *text)
{
    return sendFrame(fd, status, text, (int)strLen(text));
}

// match a command prefix, return pointer to args (after space) or NULL
static inline const char *matchCommand(const char *line, const char *cmd)
{
    int i = 0;
    while (cmd[i]) {
        if (line[i] != cmd[i]) return 0;
        i++;
    }
    if (line[i] == '\0') return line + i; // command with no args
    if (line[i] == ' ')  return line + i + 1; // skip space before args
    return 0;
}

// list every prx loaded into vsh.self. payload body is one
// "<id>\t<name>\t<filename>\n" record per module (final newline included),
// returned as a single OK frame so the host reads it all in one shot.
static void cmdModuleList(int cli)
{
    static uint32_t ids[128];
    // worst case: 128 modules * (uint32 + name + path + tabs/newline)
    static char     payload[128 * (PRX_NAME_MAX + PRX_FILENAME_MAX + 32)];
    char     name[PRX_NAME_MAX];
    char     file[PRX_FILENAME_MAX];
    uint32_t count;

    int32_t rc = prxList(ids, sizeof ids / sizeof ids[0], &count);
    if (rc < 0) {
        char err[64];
        snprintf(err, sizeof err, "prxList rc=0x%x", (unsigned)rc);
        sendReply(cli, SDB_ERR, err);
        return;
    }

    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (prxName((int32_t)ids[i], name, file) < 0) continue;
        off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                  "%u\t%s\t%s\n", (unsigned)ids[i], name, file);
    }
    if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
    if (off) sendBytes(cli, payload, (int)off);
}

// enumerate processes the bridge knows about. the bridge itself runs in
// vsh, so vsh is always present. an app process is reported when an app
// producer has registered with us — that gives us at least its name.
// payload: one "<kind>\t<name>\t<status>\n" record per process.
static void cmdProcessList(int cli)
{
    char payload[256];
    uint32_t off = 0;
    off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                              "vsh\tvsh\tlive\n");
    sys_lwmutex_lock(&serverRegistryMx, 0);
    if (appSlot.fd >= 0 && appSlot.name[0]) {
        off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                  "app\t%s\tlive\n", appSlot.name);
    }
    sys_lwmutex_unlock(&serverRegistryMx);
    if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
    if (off) sendBytes(cli, payload, (int)off);
}

// find a loaded prx by name. returns its id, or -1 if not found.
// f[] looks unused but the sys_prx_get_module_info syscall does not
// populate the name field unless filename is also requested — see
// prxInfo() in syscall.h.
static int32_t findModuleByName(const char *name)
{
    static uint32_t ids[128];
    char     n[PRX_NAME_MAX];
    char     f[PRX_FILENAME_MAX];
    uint32_t count = 0;
    if (prxList(ids, sizeof ids / sizeof ids[0], &count) < 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (prxName((int32_t)ids[i], n, f) < 0) continue;
        if (strEq(n, name)) return (int32_t)ids[i];
    }
    return -1;
}

// .lib.ent / .lib.stub record layouts — mirrored from sdk/sys/prx.h
// so we don't drag the PPU-only header into bridge code. NID tables
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

// dump exports + imports of one module. text payload:
//   id\t<id>\n
//   libent\t<addr>\t<size>\n
//   libstub\t<addr>\t<size>\n
//   seg\t<i>\t<base>\t<memsz>\n        (repeated, for context)
//   ent\t<libname>\tnfunc=<n>\tnvar=<n>\tntls=<n>\n
//     ef\t<nid>\t<opd>\n               (repeated, one per export function)
//   stub\t<libname>\tnfunc=<n>\tnvar=<n>\tntls=<n>\n
//     sf\t<nid>\t<stubOpd>\n           (repeated, one per import function)
// every pointer the walker dereferences is bounds-checked against the
// module's segments first — a bad VA inside a record will be skipped
// rather than crashing the VSH process.

// returns 1 if [addr, addr+span) lies entirely inside any one segment.
// span==0 is treated as 1 (a single addressable byte).
static int addrInSegments(uint32_t addr, uint32_t span,
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
static uint32_t copyModuleString(char *out, uint32_t outCap, uint32_t addr,
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

#define INSPECT_NFUNC_MAX  4096   // sanity cap on per-lib function count
#define INSPECT_LIBNAME_MAX  64

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
// module's segments first — a bad VA inside a record is skipped rather
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
        char err[64];
        snprintf(err, sizeof err, "prxInfo rc=0x%x", (unsigned)rc);
        sendReply(cli, SDB_ERR, err);
        return;
    }
    if (segCount > PRX_SEGMENTS_MAX) segCount = PRX_SEGMENTS_MAX;

    PrxLinkage link = {0};
    int32_t lrc = prxLinkage(id, &link, NULL, 0, NULL);
    int linkageOk = (lrc >= 0);

    // confirm linkage tables are in mapped memory before we walk them.
    // a bogus table address or a record's internal pointer would
    // otherwise fault the VSH process.
    int libentOk  = linkageOk && addrInSegments(link.libentAddr,  link.libentSize,  segs, segCount);
    int libstubOk = linkageOk && addrInSegments(link.libstubAddr, link.libstubSize, segs, segCount);
    if (linkageOk && !libentOk)  logWarn("[sdb] libent table outside segments, skipping exports\n");
    if (linkageOk && !libstubOk) logWarn("[sdb] libstub table outside segments, skipping imports\n");

    static char payload[64 * 1024];
    uint32_t off = (uint32_t)snprintf(payload, sizeof payload,
                                      "id\t%u\nname\t%s\nfile\t%s\n",
                                      (unsigned)id, name, file);
    for (uint32_t i = 0; i < segCount; i++) {
        off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                  "seg\t%llu\t0x%llx\t0x%llx\t0x%llx\t0x%llx\n",
                                  (unsigned long long)segs[i].index,
                                  (unsigned long long)segs[i].type,
                                  (unsigned long long)segs[i].base,
                                  (unsigned long long)segs[i].filesz,
                                  (unsigned long long)segs[i].memsz);
    }
    if (linkageOk) {
        off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                  "libent\t0x%x\t%u\nlibstub\t0x%x\t%u\n",
                                  (unsigned)link.libentAddr, (unsigned)link.libentSize,
                                  (unsigned)link.libstubAddr, (unsigned)link.libstubSize);
    }

    // walk exports
    if (libentOk) {
        uint32_t cursor = link.libentAddr;
        uint32_t end    = link.libentAddr + link.libentSize;
        while (cursor + sizeof(PrxLibEnt) <= end && off + 256 < sizeof payload) {
            if (!addrInSegments(cursor, sizeof(PrxLibEnt), segs, segCount)) {
                logWarn("[sdb] libent record at 0x%x outside segments\n", (unsigned)cursor);
                break;
            }
            const PrxLibEnt *e = (const PrxLibEnt *)(uintptr_t)cursor;
            if (e->structsize == 0 || e->structsize > 64) break;
            uint16_t nfunc = e->nfunc;
            if (nfunc > INSPECT_NFUNC_MAX) {
                logWarn("[sdb] ent nfunc=%u capped, libname=0x%x\n", nfunc, (unsigned)e->libname);
                nfunc = INSPECT_NFUNC_MAX;
            }
            char libname[INSPECT_LIBNAME_MAX];
            copyModuleString(libname, sizeof libname, e->libname, segs, segCount);
            off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                      "ent\t%s\tnfunc=%u\tnvar=%u\tntls=%u\n",
                                      libname, e->nfunc, e->nvar, e->ntls);

            uint32_t nidsAddr = e->nidtable;
            uint32_t addAddr  = e->addtable;
            int nidsOk = addrInSegments(nidsAddr, (uint32_t)nfunc * 4, segs, segCount);
            int addOk  = addrInSegments(addAddr,  (uint32_t)nfunc * 4, segs, segCount);
            if (nidsOk && addOk) {
                const uint32_t *nids = (const uint32_t *)(uintptr_t)nidsAddr;
                const uint32_t *add  = (const uint32_t *)(uintptr_t)addAddr;
                for (uint16_t i = 0; i < nfunc && off + 48 < sizeof payload; i++) {
                    off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                              "ef\t0x%08x\t0x%x\n",
                                              (unsigned)nids[i], (unsigned)add[i]);
                }
            } else if (nfunc > 0) {
                logWarn("[sdb] ent skipped nid/add: lib='%s' nids=0x%x(%d) add=0x%x(%d) nfunc=%u\n",
                        libname, (unsigned)nidsAddr, nidsOk, (unsigned)addAddr, addOk, nfunc);
            }
            cursor += e->structsize;
        }
    }

    // walk imports
    if (libstubOk) {
        uint32_t cursor = link.libstubAddr;
        uint32_t end    = link.libstubAddr + link.libstubSize;
        while (cursor + sizeof(PrxLibStub) <= end && off + 256 < sizeof payload) {
            if (!addrInSegments(cursor, sizeof(PrxLibStub), segs, segCount)) {
                logWarn("[sdb] libstub record at 0x%x outside segments\n", (unsigned)cursor);
                break;
            }
            const PrxLibStub *s = (const PrxLibStub *)(uintptr_t)cursor;
            if (s->structsize == 0 || s->structsize > 64) break;
            uint16_t nfunc = s->nfunc;
            if (nfunc > INSPECT_NFUNC_MAX) {
                logWarn("[sdb] stub nfunc=%u capped, libname=0x%x\n", nfunc, (unsigned)s->libname);
                nfunc = INSPECT_NFUNC_MAX;
            }
            char libname[INSPECT_LIBNAME_MAX];
            copyModuleString(libname, sizeof libname, s->libname, segs, segCount);
            off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                      "stub\t%s\tnfunc=%u\tnvar=%u\tntls=%u\n",
                                      libname, s->nfunc, s->nvar, s->ntls);

            uint32_t nidsAddr = s->funcNidtable;
            uint32_t tabAddr  = s->funcTable;
            int nidsOk = addrInSegments(nidsAddr, (uint32_t)nfunc * 4, segs, segCount);
            int tabOk  = addrInSegments(tabAddr,  (uint32_t)nfunc * 4, segs, segCount);
            if (nidsOk && tabOk) {
                const uint32_t *nids = (const uint32_t *)(uintptr_t)nidsAddr;
                const uint32_t *tab  = (const uint32_t *)(uintptr_t)tabAddr;
                for (uint16_t i = 0; i < nfunc && off + 48 < sizeof payload; i++) {
                    off += (uint32_t)snprintf(payload + off, sizeof payload - off,
                                              "sf\t0x%08x\t0x%x\n",
                                              (unsigned)nids[i], (unsigned)tab[i]);
                }
            } else if (nfunc > 0) {
                logWarn("[sdb] stub skipped nid/tab: lib='%s' nids=0x%x(%d) tab=0x%x(%d) nfunc=%u\n",
                        libname, (unsigned)nidsAddr, nidsOk, (unsigned)tabAddr, tabOk, nfunc);
            }
            cursor += s->structsize;
        }
    }

    if (sendFrameHeader(cli, SDB_OK, off) < 0) return;
    if (off) sendBytes(cli, payload, (int)off);
}


// and *outSize, returns 1. on malformed input returns 0.
static int parseNameAndSize(const char *args, char *name, int cap, uint32_t *outSize)
{
    int i = 0;
    while (args[i] && args[i] != ' ') {
        if (i >= cap - 1) return 0;
        name[i] = args[i];
        i++;
    }
    if (i == 0 || args[i] != ' ') return 0;
    name[i] = '\0';
    i++;
    uint32_t v = 0;
    int digits = 0;
    while (args[i] >= '0' && args[i] <= '9') {
        v = v * 10 + (uint32_t)(args[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0) return 0;
    *outSize = v;
    return 1;
}

// extract a path token from args. supports "quoted paths" (any chars until
// closing ") or unquoted (until first space). on success fills out (caller-
// owned, cap bytes) and returns pointer to remainder (skipping trailing ws).
// on malformed input returns NULL.
static const char *parsePath(const char *args, char *out, int cap)
{
    int o = 0;
    if (*args == '"') {
        args++;
        while (*args && *args != '"') {
            if (o >= cap - 1) return 0;
            out[o++] = *args++;
        }
        if (*args != '"') return 0;
        args++;
    } else {
        while (*args && *args != ' ') {
            if (o >= cap - 1) return 0;
            out[o++] = *args++;
        }
    }
    if (o == 0) return 0;
    out[o] = '\0';
    while (*args == ' ') args++;
    return args;
}

// parse a non-negative decimal up to the next space or end. on success fills
// *outVal and returns pointer to remainder. on malformed input returns NULL.
static const char *parseUInt64(const char *args, uint64_t *outVal)
{
    uint64_t v = 0;
    int digits = 0;
    while (*args >= '0' && *args <= '9') {
        v = v * 10 + (uint64_t)(*args - '0');
        args++;
        digits++;
    }
    if (digits == 0) return 0;
    *outVal = v;
    while (*args == ' ') args++;
    return args;
}

// get-file <path> [offset length]
//   length=0 means "until end-of-file".
//   reply: OK <window>\n<window bytes> on success, ERR ... on failure.
static void cmdGetFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    const char *rest = parsePath(args, path, sizeof path);
    if (!rest) {
        sendReply(cli, SDB_ERR, "usage: get-file <path> [offset length]");
        return;
    }

    uint64_t offset = 0, length = 0;
    if (*rest) {
        rest = parseUInt64(rest, &offset);
        if (!rest) { sendReply(cli, SDB_ERR, "bad offset"); return; }
        rest = parseUInt64(rest, &length);
        if (!rest) { sendReply(cli, SDB_ERR, "bad length"); return; }
    }

    if (!fileExists(path)) {
        sendReply(cli, SDB_ERR, "no such file");
        return;
    }
    int64_t window = fileWindowSize(path, offset, length);
    if (window < 0) {
        sendReply(cli, SDB_ERR, "offset past end of file");
        return;
    }

    if (sendFrameHeader(cli, SDB_OK, (uint32_t)window) < 0) return;

    if (window > 0 && sendFileWindow(cli, path, offset, (uint64_t)window) < 0) {
        // header already on the wire; client gets short read and reports it.
        logError("[sdb] get-file send truncated\n");
    }
}

// save-file <path> <size>\n<size raw bytes>
//   reply: OK saved <path> (<size> bytes)  or  ERR ...
//   does NOT auto-create parent directories — caller picks an existing path.
static void cmdSaveFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    const char *rest = parsePath(args, path, sizeof path);
    if (!rest) {
        sendReply(cli, SDB_ERR, "usage: save-file <path> <size>");
        return;
    }
    uint64_t size = 0;
    rest = parseUInt64(rest, &size);
    if (!rest) {
        sendReply(cli, SDB_ERR, "bad size");
        return;
    }
    if (recvFile(cli, path, (uint32_t)size) < 0) {
        sendReply(cli, SDB_ERR, "save failed");
        return;
    }
    char reply[FILE_PATH_MAX + 64];
    snprintf(reply, sizeof reply, "saved %s (%llu bytes)", path, (unsigned long long)size);
    sendReply(cli, SDB_OK, reply);
}

// row sink: write straight to the client socket.
static int captureSendRow(const void *row, uint32_t bytes, void *user)
{
    return sendBytes(*(int *)user, row, (int)bytes);
}

// capture <x> <y> <w> <h>
//   streams w*h*4 bytes of ARGB8888 (vram byte order) after the OK header.
//   1x1 covers the single-pixel case. region is clipped to display bounds.
//   refused while a game/app owns rsx — pausing the fifo then would wedge
//   the gpu (we only touch vram while xmb is the foreground compositor).
static void cmdCapture(int cli, const char *args)
{
    uint64_t x = 0, y = 0, w = 0, h = 0;
    const char *r = parseUInt64(args, &x);
    if (r) r = parseUInt64(r, &y);
    if (r) r = parseUInt64(r, &w);
    if (r) r = parseUInt64(r, &h);
    if (!r) { sendReply(cli, SDB_ERR, "usage: capture <x> <y> <w> <h>"); return; }

    if (!isXmbReady()) { sendReply(cli, SDB_ERR, "capture: xmb not in foreground"); return; }

    uint32_t dw, dh, pitch, depth;
    captureDisplayInfo(&dw, &dh, &pitch, &depth);
    if (w == 0 || h == 0 || x >= dw || y >= dh) {
        sendReply(cli, SDB_ERR, "out of bounds"); return;
    }
    uint32_t cw = (x + w > dw) ? (dw - (uint32_t)x) : (uint32_t)w;
    uint32_t ch = (y + h > dh) ? (dh - (uint32_t)y) : (uint32_t)h;

    if (sendFrameHeader(cli, SDB_OK, cw * ch * 4) < 0) return;
    captureRegion((uint32_t)x, (uint32_t)y, cw, ch, captureSendRow, &cli);
}

// delete-file <path>
//   reply: OK deleted <path>  or  ERR ...
//   missing files are treated as success (idempotent).
static void cmdDeleteFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    if (!parsePath(args, path, sizeof path)) {
        sendReply(cli, SDB_ERR, "usage: delete-file <path>");
        return;
    }
    if (deleteFile(path) < 0) {
        sendReply(cli, SDB_ERR, "delete failed");
        return;
    }
    char reply[FILE_PATH_MAX + 64];
    snprintf(reply, sizeof reply, "deleted %s", path);
    sendReply(cli, SDB_OK, reply);
}

// list-dir <path>
//   reply: OK <n>\n<n bytes> with one "<kind>\t<size>\t<mtime>\t<name>\n"
//   line per entry. used to diff /dev_hdd0/ before and after a sony-side
//   install so we can find what xmb registration writes that we don't.
static void cmdListDir(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    if (!parsePath(args, path, sizeof path)) {
        sendReply(cli, SDB_ERR, "usage: list-dir <path>");
        return;
    }
    static char body[64 * 1024];
    int n = listDir(path, body, (int)sizeof body);
    if (n < 0) {
        sendReply(cli, SDB_ERR, "list failed");
        return;
    }
    if (sendFrameHeader(cli, SDB_OK, (uint32_t)n) < 0) return;
    if (n > 0) sendBytes(cli, body, n);
}

// dispatch one command from the client. returns 0 on success, -1 on send
// failure (caller should drop the connection). power commands fire the
// syscall directly — lv2 tears the whole system down.
static int dispatchCommand(int cli, char *buf)
{
    // skip logging ping — client polls it every few seconds and would
    // otherwise fill dbg.txt with noise.
    if (!matchCommand(buf, "ping")) {
        logInfo("[sdb] cmd: %s\n", buf);
    }

    if (matchCommand(buf, "ping")) {
        return sendReply(cli, SDB_OK, "");
    }
    // vsh flags an improper shutdown on next boot unless /dev_hdd0/tmp/turnoff
    // is removed before sys_sm_shutdown. xai_plugin / evilnat cfw power options
    // do the same thing — see apps/xai_plugin/xai_plugin/functions.cpp rebootXMB().
    if (matchCommand(buf, "restart-ps3")) {
        sendReply(cli, SDB_OK, "rebooting");
        cellFsUnlink("/dev_hdd0/tmp/turnoff");
        sysPower(POWER_REBOOT);
        return 0;
    }
    if (matchCommand(buf, "restart-xmb")) {
        sendReply(cli, SDB_OK, "restarting xmb");
        cellFsUnlink("/dev_hdd0/tmp/turnoff");
        sysPower(POWER_VSH_REBOOT);
        return 0;
    }
    if (matchCommand(buf, "shutdown")) {
        sendReply(cli, SDB_OK, "shutting down");
        cellFsUnlink("/dev_hdd0/tmp/turnoff");
        sysPower(POWER_SHUTDOWN);
        return 0;
    }
    if (matchCommand(buf, "display-info")) {
        uint32_t w, h, pitch, depth;
        char     reply[96];
        captureDisplayInfo(&w, &h, &pitch, &depth);
        snprintf(reply, sizeof reply, "%u %u %u %u",
                 (unsigned)w, (unsigned)h, (unsigned)pitch, (unsigned)depth);
        return sendReply(cli, SDB_OK, reply);
    }
    if (matchCommand(buf, "module-list")) {
        cmdModuleList(cli);
        return 0;
    }
    if (matchCommand(buf, "process-list")) {
        cmdProcessList(cli);
        return 0;
    }

    const char *args;
    char  name[PLUGIN_NAME_MAX];
    uint32_t size;
    char  reply[128];

    if ((args = matchCommand(buf, "module-info")) != 0) {
        cmdModuleInfo(cli, args);
    }
    else if ((args = matchCommand(buf, "get-file")) != 0) {
        cmdGetFile(cli, args);
    }
    else if ((args = matchCommand(buf, "save-file")) != 0) {
        cmdSaveFile(cli, args);
    }
    else if ((args = matchCommand(buf, "delete-file")) != 0) {
        cmdDeleteFile(cli, args);
    }
    else if ((args = matchCommand(buf, "list-dir")) != 0) {
        cmdListDir(cli, args);
    }
    else if ((args = matchCommand(buf, "capture")) != 0) {
        cmdCapture(cli, args);
    }
    else if ((args = matchCommand(buf, "vsh-plugin-install")) != 0) {
        if (!parseNameAndSize(args, name, sizeof name, &size)) {
            sendReply(cli, SDB_ERR, "usage: vsh-plugin-install <name> <size>");
        } else if (installPlugin(cli, name, size) < 0) {
            sendReply(cli, SDB_ERR, "install failed");
        } else {
            snprintf(reply, sizeof reply, "installed %s (%u bytes)", name, (unsigned)size);
            sendReply(cli, SDB_OK, reply);
        }
    }
    else if ((args = matchCommand(buf, "vsh-plugin-uninstall")) != 0) {
        if (args[0] == '\0') {
            sendReply(cli, SDB_ERR, "usage: vsh-plugin-uninstall <name>");
        } else if (uninstallPlugin(args) < 0) {
            sendReply(cli, SDB_ERR, "uninstall failed");
        } else {
            snprintf(reply, sizeof reply, "uninstalled %s", args);
            sendReply(cli, SDB_OK, reply);
        }
    }
    else if ((args = matchCommand(buf, "pkg-uninstall")) != 0) {
        if (!isValidTitleId(args)) {
            sendReply(cli, SDB_ERR, "usage: pkg-uninstall <TITLE_ID>");
        } else {
            uint64_t freed = 0;
            int rc = uninstallPkg(args, &freed);
            if (rc < 0) {
                sendReply(cli, SDB_ERR, "uninstall failed");
            } else if (rc > 0) {
                sendReply(cli, SDB_ERR, "not installed");
            } else {
                snprintf(reply, sizeof reply, "uninstalled %s (%llu bytes)",
                         args, (unsigned long long)freed);
                sendReply(cli, SDB_OK, reply);
            }
        }
    }
    else if ((args = matchCommand(buf, "pkg-install")) != 0) {
        // wire: "pkg-install <name> <clean> <size>". host shapes the URL
        // params; HttpBridge appends `<size>` from the POST body length.
        char  pkgName[SDB_NAME_MAX];
        uint32_t clean = 0;
        // reuse parseNameAndSize for the leading "<name> <clean>" pair,
        // then take the trailing size by hand.
        if (!parseNameAndSize(args, pkgName, sizeof pkgName, &clean)) {
            sendReply(cli, SDB_ERR, "usage: pkg-install <name> <clean> <size>");
        } else {
            const char *tail = args;
            while (*tail && *tail != ' ') tail++;          // skip name
            while (*tail == ' ') tail++;
            while (*tail && *tail != ' ') tail++;          // skip clean
            while (*tail == ' ') tail++;
            uint32_t size = 0;
            int digits = 0;
            while (*tail >= '0' && *tail <= '9') { size = size * 10 + (uint32_t)(*tail - '0'); tail++; digits++; }
            if (digits == 0) {
                sendReply(cli, SDB_ERR, "usage: pkg-install <name> <clean> <size>");
            } else if (stagePkgUpload(cli, pkgName, size) < 0) {
                sendReply(cli, SDB_ERR, "stage failed");
            } else {
                // staged ok — extract into /dev_hdd0/game/<TITLE_ID>/. read
                // title-id from the pkg's PARAM.SFO so the host never has to
                // know it. `clean` controls whether an existing install is
                // wiped first (matches xmb "reinstall" behavior).
                char pkgPath[FILE_PATH_MAX];
                buildStagePath(pkgPath, sizeof pkgPath, pkgName);
                char     titleId[PKG_TITLE_LEN + 1] = {0};
                uint32_t files = 0;
                uint64_t bytes = 0;
                if (installPkg(pkgPath, (int)clean, titleId, &files, &bytes) < 0) {
                    sendReply(cli, SDB_ERR, "extract failed");
                } else {
                    snprintf(reply, sizeof reply, "installed %s (%u files, %llu bytes)",
                             titleId, (unsigned)files, (unsigned long long)bytes);
                    sendReply(cli, SDB_OK, reply);
                }
            }
        }
    }
    else {
        sendReply(cli, SDB_ERR, "unknown command");
    }
    return 0;
}

// persistent host session: read framed commands (newline-terminated) and
// send framed replies until the host disconnects or a send fails. each
// dispatch grabs the host write mutex so a producer LOG forward cannot
// splice bytes inside the reply frame.
static void handleHostSession(int cli)
{
    char buf[SDB_BUF_MAX];
    while (isServerRunning) {
        int len = receiveLine(cli, buf, sizeof buf);
        if (len <= 0) return;
        sys_lwmutex_lock(&serverHostWriteMx, 0);
        int rc = dispatchCommand(cli, buf);
        sys_lwmutex_unlock(&serverHostWriteMx);
        if (rc < 0) return;
    }
}

static inline void copyProducerName(ProducerSlot *slot, const char *name)
{
    int i = 0;
    while (i < SDB_NAME_MAX - 1 && name[i]) { slot->name[i] = name[i]; i++; }
    slot->name[i] = '\0';
}

// register a producer in the app slot or a free plugin slot. on app
// registration any previous app socket is shut down (newest wins). returns
// the slot pointer, or NULL if no plugin slot was free.
static ProducerSlot *registerProducer(int cli, int isApp, const char *name)
{
    ProducerSlot *slot = 0;
    sys_lwmutex_lock(&serverRegistryMx, 0);

    if (isApp) {
        if (appSlot.fd >= 0) {
            logWarn("[sdb] replacing app producer %s with %s\n", appSlot.name, name);
            shutdown(appSlot.fd, SHUT_RDWR);
        }
        appSlot.fd = cli;
        copyProducerName(&appSlot, name);
        slot = &appSlot;
    } else {
        for (int i = 0; i < SDB_MAX_PLUGINS; i++) {
            if (pluginSlots[i].fd < 0) {
                pluginSlots[i].fd = cli;
                copyProducerName(&pluginSlots[i], name);
                slot = &pluginSlots[i];
                break;
            }
        }
    }

    sys_lwmutex_unlock(&serverRegistryMx);
    return slot;
}

static void unregisterProducer(ProducerSlot *slot)
{
    sys_lwmutex_lock(&serverRegistryMx, 0);
    slot->fd = -1;
    slot->name[0] = '\0';
    sys_lwmutex_unlock(&serverRegistryMx);
}

// producer session: forward incoming "LOG <n>\n<bytes>" frames to the host
// (if connected). also accepts "BYE\n" as a graceful disconnect signal.
static void handleProducerSession(int cli, ProducerSlot *slot)
{
    char buf[SDB_BUF_MAX];
    while (isServerRunning) {
        int len = receiveLine(cli, buf, sizeof buf);
        if (len <= 0) return;

        const char *args;
        if ((args = matchCommand(buf, "LOG")) != 0) {
            uint64_t n = 0;
            if (parseUInt64(args, &n) == 0 || n == 0 || n > SDB_LOG_BODY_MAX) {
                logWarn("[sdb] producer %s: bad LOG frame\n", slot->name);
                return;
            }
            char body[SDB_LOG_BODY_MAX];
            if (receiveExact(cli, body, (int)n) < 0) return;
            forwardLogToHost(body, (int)n);
        }
        else if (matchCommand(buf, "BYE")) {
            return;
        }
        else {
            logWarn("[sdb] producer %s: unknown frame: %s\n", slot->name, buf);
        }
    }
}

// per-connection thread arg: just the fd boxed as a uint64_t.
static void runConnHandler(uint64_t arg)
{
    int cli = (int)arg;

    // first line decides the role of this socket. expected forms:
    //   "REGISTER plugin <name>\n"  — producer plugin
    //   "REGISTER app <name>\n"     — producer app
    //   anything else               — legacy host control session
    char first[SDB_BUF_MAX];
    int len = receiveLine(cli, first, sizeof first);
    if (len <= 0) goto done;

    const char *args = matchCommand(first, "REGISTER");
    if (args) {
        int isApp = 0;
        const char *name = 0;
        if ((name = matchCommand(args, "app")) != 0)    isApp = 1;
        else if ((name = matchCommand(args, "plugin")) != 0) isApp = 0;

        if (!name || !*name) {
            logWarn("[sdb] bad REGISTER frame: %s\n", first);
            goto done;
        }
        ProducerSlot *slot = registerProducer(cli, isApp, name);
        if (!slot) {
            logWarn("[sdb] no producer slot for %s\n", name);
            goto done;
        }
        // producers only push LOG frames outbound and may go minutes
        // between writes; the host-oriented 30s recv timeout from the
        // accept loop would silently drop the slot. clear it so the
        // producer stays registered until it actively disconnects.
        struct timeval none = { 0, 0 };
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
        logInfo("[sdb] producer registered: %s %s\n", isApp ? "app" : "plugin", name);
        handleProducerSession(cli, slot);
        unregisterProducer(slot);
        logInfo("[sdb] producer disconnected: %s\n", name);
    }
    else {
        // single-host policy: reject a second host while one is connected.
        if (serverHostFd >= 0) {
            logWarn("[sdb] rejecting second host (already connected)\n");
            sendReply(cli, SDB_ERR, "busy");
        } else {
            serverHostFd = cli;
            // drain any logs buffered while no host was connected (plugin
            // startup, bridge startup, producer registrations) so the
            // Debug Logs tab shows the full history, not just post-connect.
            sys_lwmutex_lock(&serverHostWriteMx, 0);
            drainBacklog(cli);
            sys_lwmutex_unlock(&serverHostWriteMx);
            logInfo("[sdb] host connected\n");
            // the first line we already consumed is a real command; dispatch
            // it before entering the read loop.
            sys_lwmutex_lock(&serverHostWriteMx, 0);
            int rc = dispatchCommand(cli, first);
            sys_lwmutex_unlock(&serverHostWriteMx);
            if (rc >= 0) handleHostSession(cli);
            serverHostFd = -1;
            logInfo("[sdb] host disconnected\n");
        }
    }

done:
    shutdown(cli, SHUT_RDWR);
    socketclose(cli);
    sys_ppu_thread_exit(0);
}

// open a listening tcp socket on the given port, or -1 on any failure.
// caller retries the whole thing — keeps socket+bind+listen atomic so we
// never end up with a half-initialised fd lingering across retries.
static int openListener(uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a;
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { socketclose(s); return -1; }
    if (listen(s, 2) < 0)                             { socketclose(s); return -1; }
    return s;
}

static void runAcceptLoop(uint64_t arg)
{
    (void)arg;
    logInfo("[sdb] server thread start\n");

    // wait for xmb readiness before binding
    int ticks = 0;
    while (!isXmbReady()) {
        if (!isServerRunning) {
            logInfo("[sdb] cancelled during xmb wait\n");
            sys_ppu_thread_exit(0);
            return;
        }
        sys_timer_sleep(1);
        if (++ticks > 60) {
            logError("[sdb] xmb ready timeout\n");
            sys_ppu_thread_exit(0);
            return;
        }
    }
    logInfo("[sdb] xmb ready\n");

    int fd      = -1;
    int retries = 0;
    while (fd < 0 && retries < 30) {
        if (!isServerRunning) {
            sys_ppu_thread_exit(0);
            return;
        }
        fd = openListener(SDB_PORT);
        if (fd < 0) {
            logWarn("[sdb] listen failed, retrying\n");
            sys_timer_sleep(2);
            retries++;
        }
    }
    if (fd < 0) {
        logError("[sdb] giving up — port %d unavailable\n", SDB_PORT);
        sys_ppu_thread_exit(0);
        return;
    }

    // 1-second timeout so accept() wakes periodically to check isServerRunning
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    serverListenFd = fd;
    logInfo("[sdb] listening on :%d\n", SDB_PORT);

    while (isServerRunning) {
        struct sockaddr_in ra;
        socklen_t al = sizeof ra;
        int c = accept(fd, (struct sockaddr *)&ra, &al);
        if (c < 0) continue;
        if (!isServerRunning) {
            shutdown(c, SHUT_RDWR);
            socketclose(c);
            break;
        }
        // per-client recv timeout so a wedged peer (e.g. powered off mid-
        // session) doesn't keep the slot indefinitely; the connection will
        // be dropped and the next reconnect attempt accepted.
        struct timeval ct;
        ct.tv_sec  = 30;
        ct.tv_usec = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &ct, sizeof ct);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof ct);

        // detached per-connection thread. handshake decides host vs producer
        // role; host single-client enforcement lives inside the thread so
        // producers can keep registering even while a host is connected.
        sys_ppu_thread_t tid = 0;
        spawnThread(&tid, runConnHandler, (uint64_t)c, THREAD_STACK_SIZE_16KB, "sdb_conn");
        // detached — thread self-exits on disconnect and lv2 reclaims it.
    }

    socketclose(fd);
    serverListenFd = -1;
    logInfo("[sdb] server thread exit\n");
    sys_ppu_thread_exit(0);
}

// initialize state and mutexes, then spawn the accept-loop thread.
static void startServer(void)
{
    isServerRunning = 1;

    sys_lwmutex_attribute_t a;
    sys_lwmutex_attribute_initialize(a);
    // recursive: dispatchCommand runs under the host write mutex, and any
    // logInfo inside it re-enters via the bridge's own LOG tee. without
    // recursion that's an instant self-deadlock.
    a.attr_recursive = SYS_SYNC_RECURSIVE;
    sys_lwmutex_create(&serverHostWriteMx, &a);
    a.attr_recursive = SYS_SYNC_NOT_RECURSIVE;
    sys_lwmutex_create(&serverRegistryMx, &a);

    appSlot.fd = -1;
    appSlot.name[0] = '\0';
    for (int i = 0; i < SDB_MAX_PLUGINS; i++) {
        pluginSlots[i].fd = -1;
        pluginSlots[i].name[0] = '\0';
    }

    // accept loop thread is joinable so stopServer can wait for clean
    // teardown; per-connection threads (above) are detached.
    sys_ppu_thread_t tid = 0;
    spawnJoinableThread(&tid, runAcceptLoop, 0, THREAD_STACK_SIZE_16KB, "sdb");
    serverThreadId = tid;
}

// stop the server and wait for the thread to exit. only called from _stop
// (sys_prx_stop_module path) — never from the server thread itself, so no
// self-join guard needed.
static void stopServer(void)
{
    logInfo("[sdb] stopServer\n");
    isServerRunning = 0;

    // wake any in-progress host recv
    if (serverHostFd >= 0) {
        shutdown(serverHostFd, SHUT_RDWR);
    }
    // wake any in-progress producer recvs
    sys_lwmutex_lock(&serverRegistryMx, 0);
    if (appSlot.fd >= 0) shutdown(appSlot.fd, SHUT_RDWR);
    for (int i = 0; i < SDB_MAX_PLUGINS; i++) {
        if (pluginSlots[i].fd >= 0) shutdown(pluginSlots[i].fd, SHUT_RDWR);
    }
    sys_lwmutex_unlock(&serverRegistryMx);
    // shutdown listen socket to wake accept() immediately
    if (serverListenFd >= 0) {
        shutdown(serverListenFd, SHUT_RDWR);
    }

    if (serverThreadId != 0) {
        uint64_t exitCode;
        sys_ppu_thread_join(serverThreadId, &exitCode);
        serverThreadId = 0;
    }

    sys_lwmutex_destroy(&serverHostWriteMx);
    sys_lwmutex_destroy(&serverRegistryMx);

    logInfo("[sdb] stopServer done\n");
}
