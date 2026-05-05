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

/* Append one timestamped line to /dev_hdd0/tmp/dbg.txt.
 * Variadic: plain string ("hello\n") or printf-style ("rc=0x%x\n", rc).
 * Full C99 printf minus floats (see common/printf.h).
 *
 * Thread-safety: builds the timestamp + message into one stack buffer and
 * issues a SINGLE cellFsWrite. With O_APPEND the kernel positions and writes
 * atomically per call, so concurrent loggers from different threads/PRX's
 * cannot interleave their timestamp/body halves on the same line.
 *
 * Buffer is 1 KB for the message (any extra is silently dropped) plus 22
 * bytes for the timestamp. */
static inline void dbgLog(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline void dbgLog(const char *fmt, ...)
{
    char line[1024 + 24];

    int tn = dbgFormatTimestamp(line);   /* writes up to 22 bytes, no NUL */

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + tn, sizeof line - tn, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    int max = (int)sizeof line - tn - 1;
    if (n > max) n = max;

    int fd;
    if (cellFsOpen(DBG_LOG, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                   &fd, NULL, 0) == CELL_FS_SUCCEEDED)
    {
        uint64_t written;
        cellFsWrite(fd, line, (uint64_t)(tn + n), &written);
        cellFsClose(fd);
    }
}
