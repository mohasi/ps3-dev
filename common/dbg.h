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
 * Buffer is 1 KB on the stack — well above any real log line; if we ever
 * do dump something longer the excess is silently dropped. */
static inline void dbgLog(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline void dbgLog(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof msg) n = (int)sizeof msg - 1;

    int fd;
    if (cellFsOpen(DBG_LOG, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                   &fd, NULL, 0) == CELL_FS_SUCCEEDED)
    {
        uint64_t written;
        char ts[24];
        int tn = dbgFormatTimestamp(ts);
        if (tn > 0) cellFsWrite(fd, ts, tn, &written);
        cellFsWrite(fd, msg, (uint64_t)n, &written);
        cellFsClose(fd);
    }
}
