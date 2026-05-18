#pragma once

#include <stdarg.h>
#include <stdint.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/rtc.h>   /* local-time source — librtc_stub, PRX-safe.
                         * Callers must link -lrtc_stub. */

#include "printf.h"     /* vsnprintf from common/printf.c — must NOT use
                         * libc_stub's printf family from a VSH PRX;
                         * see common/printf.h for rationale. */

#define DBG_LOG "/dev_hdd0/tmp/dbg.txt"

/* Optional live-log sink. If set, every logInfo/Warn/Error call hands the
 * fully-formatted line (timestamp + level + body) to the sink AFTER the
 * file write. Unset == file only. Per-PRX static — each plugin owns its
 * own sink. simple-debug-bridge's bridge.h wires this to push to the host. */
typedef void (*LogSinkFn)(const char *line, int len);
static LogSinkFn logSink = 0;

static inline void setLogSink(LogSinkFn fn) { logSink = fn; }

/* Shared bounded-line struct used by every pre-connect log ring in this
 * codebase (the producer-side ring in bridge.h and the bridge-side ring
 * in server.h). One typedef, one max-line constant — no parallel copies. */
#define LOG_LINE_MAX 256

typedef struct {
    int  len;
    char data[LOG_LINE_MAX];
} BacklogLine;

/* Format "[YYYY-MM-DD HH:MM:SS] " into out (22 chars, no NUL). Returns the
 * byte count, or 0 if the RTC query fails — caller just skips the prefix
 * rather than dropping the log line. */
static inline int dbgFormatTimestamp(char *out)
{
    CellRtcDateTime d;
    if (cellRtcGetCurrentClockLocalTime(&d) != 0) return 0;

    int y  = (int)d.year;   if (y  < 0 || y  > 9999) y  = 0;
    int mo = (int)d.month;  if (mo < 1 || mo > 12)   mo = 0;
    int da = (int)d.day;    if (da < 1 || da > 31)   da = 0;
    int hh = (int)d.hour;   if (hh < 0 || hh > 23)   hh = 0;
    int mi = (int)d.minute; if (mi < 0 || mi > 59)   mi = 0;
    int se = (int)d.second; if (se < 0 || se > 59)   se = 0;

    int o = 0;
    out[o++] = '[';
    out[o++] = (char)('0' + (y  / 1000) % 10);
    out[o++] = (char)('0' + (y  /  100) % 10);
    out[o++] = (char)('0' + (y  /   10) % 10);
    out[o++] = (char)('0' +  y          % 10);
    out[o++] = '-';
    out[o++] = (char)('0' + (mo / 10) % 10);
    out[o++] = (char)('0' +  mo        % 10);
    out[o++] = '-';
    out[o++] = (char)('0' + (da / 10) % 10);
    out[o++] = (char)('0' +  da        % 10);
    out[o++] = ' ';
    out[o++] = (char)('0' + (hh / 10) % 10);
    out[o++] = (char)('0' +  hh        % 10);
    out[o++] = ':';
    out[o++] = (char)('0' + (mi / 10) % 10);
    out[o++] = (char)('0' +  mi        % 10);
    out[o++] = ':';
    out[o++] = (char)('0' + (se / 10) % 10);
    out[o++] = (char)('0' +  se        % 10);
    out[o++] = ']';
    out[o++] = ' ';
    return o;   /* == 22 */
}

/* Append one timestamped, level-prefixed line to /dev_hdd0/tmp/dbg.txt.
 * Variadic: plain string ("hello\n") or printf-style ("rc=0x%x\n", rc).
 * Full C99 printf minus floats (see common/printf.h).
 *
 * Format: "[YYYY-MM-DD HH:MM:SS] [LVL ] <message>"
 *         where LVL is INFO / WARN / ERR  (5-char fixed width incl. space).
 *
 * Thread-safety: builds the timestamp + level + message into one stack
 * buffer and issues a SINGLE cellFsWrite. With O_APPEND the kernel
 * positions and writes atomically per call, so concurrent loggers from
 * different threads/PRX's cannot interleave halves on the same line.
 *
 * Buffer is 1 KB for the message (any extra is silently dropped) plus 22
 * bytes for the timestamp and 7 for the level tag. */
static inline void logEmit(const char *level, const char *fmt, va_list ap)
{
    char line[1024 + 32];

    int o = dbgFormatTimestamp(line);    /* up to 22 bytes, no NUL */

    /* "[LVL ] " — 7 bytes, fixed width so log columns align. */
    line[o++] = '[';
    line[o++] = level[0];
    line[o++] = level[1];
    line[o++] = level[2];
    line[o++] = level[3];
    line[o++] = ']';
    line[o++] = ' ';

    int n = vsnprintf(line + o, sizeof line - o, fmt, ap);
    if (n <= 0) return;
    int max = (int)sizeof line - o - 1;
    if (n > max) n = max;

    int fd;
    if (cellFsOpen(DBG_LOG, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                   &fd, NULL, 0) == CELL_FS_SUCCEEDED)
    {
        uint64_t written;
        cellFsWrite(fd, line, (uint64_t)(o + n), &written);
        cellFsClose(fd);
    }

    if (logSink) logSink(line, o + n);
}

static inline void logInfo(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static inline void logWarn(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static inline void logError(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline void logInfo(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    logEmit("INFO", fmt, ap);
    va_end(ap);
}

static inline void logWarn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    logEmit("WARN", fmt, ap);
    va_end(ap);
}

static inline void logError(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    logEmit("ERR ", fmt, ap);
    va_end(ap);
}
