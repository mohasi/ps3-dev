#pragma once

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

// Reads entire file into buf, NUL-terminates. Returns bytes read, or -1 on failure.
static int readFile(const char *path, char *buf, int cap)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
    uint64_t bytesRead = 0;
    int r = cellFsRead(fd, buf, (uint64_t)(cap - 1), &bytesRead);
    cellFsClose(fd);
    if (r != CELL_FS_SUCCEEDED || bytesRead == 0) return -1;
    buf[bytesRead] = '\0';
    return (int)bytesRead;
}

// Returns 1 if file exists, 0 otherwise.
static int fileExists(const char *path)
{
    CellFsStat st;
    return cellFsStat(path, &st) == CELL_FS_SUCCEEDED;
}

// Writes len bytes to path, creating/truncating. Returns 0 on success.
static int writeFile(const char *path, const char *data, uint64_t len)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
    uint64_t written = 0;
    int r = cellFsWrite(fd, data, len, &written);
    cellFsClose(fd);
    return (r == CELL_FS_SUCCEEDED && written == len) ? 0 : -1;
}

// Creates a directory. Returns 0 if created or already exists.
static int makeDir(const char *path)
{
    int r = cellFsMkdir(path, CELL_FS_S_IFDIR | 0777);
    return (r == CELL_FS_SUCCEEDED || r == (int)CELL_FS_EEXIST) ? 0 : r;
}
