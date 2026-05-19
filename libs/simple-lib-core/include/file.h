#pragma once

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

// Reads entire file into buf, NUL-terminates. Returns bytes read, or -1 on failure.
static inline int readFile(const char *path, char *buf, int cap)
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
static inline int fileExists(const char *path)
{
    CellFsStat st;
    return cellFsStat(path, &st) == CELL_FS_SUCCEEDED;
}

// Writes len bytes to path, creating/truncating. Returns 0 on success.
static inline int writeFile(const char *path, const char *data, uint64_t len)
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
static inline int makeDir(const char *path)
{
    int r = cellFsMkdir(path, CELL_FS_S_IFDIR | 0777);
    return (r == CELL_FS_SUCCEEDED || r == (int)CELL_FS_EEXIST) ? 0 : r;
}

// Deletes a file. Returns 0 on success or if the file did not exist (idempotent).
static inline int deleteFile(const char *path)
{
    int r = cellFsUnlink(path);
    return (r == CELL_FS_SUCCEEDED || r == (int)CELL_FS_ENOENT) ? 0 : -1;
}

// Recursively deletes path (file or directory). Accumulates the size of
// every removed regular file into *bytesFreed (pass NULL to ignore).
// Returns 0 on success, 0 if path did not exist (idempotent), -1 on failure.
static inline int deleteTree(const char *path, uint64_t *bytesFreed)
{
    CellFsStat st;
    int r = cellFsStat(path, &st);
    if (r == (int)CELL_FS_ENOENT) return 0;
    if (r != CELL_FS_SUCCEEDED)   return -1;

    if (!(st.st_mode & CELL_FS_S_IFDIR)) {
        if (bytesFreed) *bytesFreed += st.st_size;
        return deleteFile(path);
    }

    int fd;
    if (cellFsOpendir(path, &fd) != CELL_FS_SUCCEEDED) return -1;

    char child[512];
    CellFsDirent ent;
    uint64_t n;
    while (cellFsReaddir(fd, &ent, &n) == CELL_FS_SUCCEEDED && n > 0) {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
            (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) continue;
        int len = 0;
        while (path[len] && len < (int)sizeof child - 2) { child[len] = path[len]; len++; }
        if (len > 0 && child[len - 1] != '/' && len < (int)sizeof child - 1) child[len++] = '/';
        for (int i = 0; ent.d_name[i] && len < (int)sizeof child - 1; i++) child[len++] = ent.d_name[i];
        child[len] = '\0';
        if (deleteTree(child, bytesFreed) < 0) { cellFsClosedir(fd); return -1; }
    }
    cellFsClosedir(fd);

    return cellFsRmdir(path) == CELL_FS_SUCCEEDED ? 0 : -1;
}
