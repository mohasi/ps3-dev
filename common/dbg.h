#pragma once

#include <cell/fs/cell_fs_file_api.h>

#define DBG_LOG "/dev_hdd0/tmp/dbg.txt"

static inline int dbgStrLen(const char *s) { int n = 0; while (*s++) n++; return n; }

static void dbgLog(const char *text)
{
    int fd;
    if (cellFsOpen(DBG_LOG, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                   &fd, NULL, 0) == CELL_FS_SUCCEEDED)
    {
        uint64_t written;
        cellFsWrite(fd, text, dbgStrLen(text), &written);
        cellFsClose(fd);
    }
}
