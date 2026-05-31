#pragma once

// shared scratch + small primitives used by every cmd-*.h handler.
// included exactly once via server.h -> prx.c, so the `static`
// storage here lives in a single TU and the buffers are unique.

#include <stdint.h>

#include "dbg.h"
#include "syscall.h"
#include "string-utilities.h"
#include "printf.h"
#include "thread.h"
#include "wire.h"

#define SDB_OK            "OK"
#define SDB_ERR           "ERR"

// shared reply assembly buffer + loaded-module id list. only one command
// runs at a time on the host socket (single-threaded dispatch loop), so
// every handler reuses these instead of carrying its own large static
// buffer. sized for the biggest payload we emit: cmdProcessInfo for vsh
// (~70 KiB, dominated by ~4400 main-exe export NIDs). producer threads
// never touch these - they go through forwardLogToHost with its own
// tiny frame buffer.
#define REPLY_BUF_BYTES   (96 * 1024)
#define MODULE_IDS_MAX     128
static char     replyBuf[REPLY_BUF_BYTES];
static uint32_t moduleIds[MODULE_IDS_MAX];

// every reply on the wire is "<status> <n>\n[<n bytes>]" where status is
// OK or ERR and n is the decimal payload length. n=0 is valid ("OK 0\n").
// the host reads the header line then exactly n bytes - no other framing.
//
//   sendReply       - c-string payload, length figured out internally.
//                     covers empty replies (""), errors, and every text OK.
//   sendFrameHeader - write just the header for a known-size payload; caller
//                     follows up with sendBytes / sendFileWindow / captureRegion
//                     to produce the n bytes (capture, pull-file, module-list).
static inline int sendReply(int fd, const char *status, const char *text)
{
    return sendFrame(fd, status, text, (int)strLen(text));
}

// shorthand for "<label> rc=0x<hex>" error replies. used by every
// command handler that fails on a syscall return code.
static inline int sendErrRc(int fd, const char *label, int32_t rc)
{
    char err[64];
    snprintf(err, sizeof err, "%s rc=0x%x", label, (unsigned)rc);
    return sendReply(fd, SDB_ERR, err);
}

// match a command prefix, return pointer to args (after space) or NULL
static inline const char *matchCommand(const char *line, const char *cmd)
{
    int i = 0;
    while (cmd[i]) {
        if (line[i] != cmd[i]) return 0;
        i++;
    }
    if (line[i] == '\0') return line + i;     // command with no args
    if (line[i] == ' ')  return line + i + 1; // skip space before args
    return 0;
}

// parse "<name> <decimal>" into the caller's name[] (cap bytes) and
// *outSize. returns 1 on success, 0 on malformed input.
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

// find a loaded prx by name. returns its id, or -1 if not found.
// f[] looks unused but the sys_prx_get_module_info syscall does not
// populate the name field unless filename is also requested - see
// prxInfo() in syscall.h.
static int32_t findModuleByName(const char *name)
{
    char     n[PRX_NAME_MAX];
    char     f[PRX_FILENAME_MAX];
    uint32_t count = 0;
    if (prxList(moduleIds, MODULE_IDS_MAX, &count) < 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        int32_t id = (int32_t)moduleIds[i];
        // yield between prxName calls. without this, the kernel state
        // touched by prxName never gets a scheduling gap and the next
        // arming-path syscall (prxInfo/prxLinkage in cmdModuleTraceOn)
        // observes it inconsistently and wedges. previously the
        // per-iteration logInfo() was incidentally providing this yield
        // via its file-write syscall; the explicit yield removes that
        // hidden dependency.
        yieldThread();
        if (prxName(id, n, f) < 0) continue;
        if (strEq(n, name)) return id;
    }
    return -1;
}

