#pragma once

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include "string-utilities.h"

#define MAX_PATH_LEN 512

// lv2 syscall 837. idempotent -- second call returns an error that is safe
// to ignore. raw inline asm so this works from vsh prx where libc/sony
// stubs are not linked.
static inline int64_t mountDevBlind(void)
{
    register uint64_t r3  __asm__("3")  = (uint64_t)(uintptr_t)"CELL_FS_IOS:BUILTIN_FLSH1";
    register uint64_t r4  __asm__("4")  = (uint64_t)(uintptr_t)"CELL_FS_FAT";
    register uint64_t r5  __asm__("5")  = (uint64_t)(uintptr_t)"/dev_blind";
    register uint64_t r6  __asm__("6")  = 0;
    register uint64_t r7  __asm__("7")  = 0;
    register uint64_t r8  __asm__("8")  = 0;
    register uint64_t r9  __asm__("9")  = 0;
    register uint64_t r10 __asm__("10") = 0;
    register uint64_t r11 __asm__("11") = 837;

    __asm__ volatile ("sc\n"
        : "+r"(r3)
        : "r"(r4), "r"(r5), "r"(r6), "r"(r7),
          "r"(r8), "r"(r9), "r"(r10), "r"(r11)
        : "r0", "r12", "cr0", "ctr", "xer", "memory");
    return (int64_t)r3;
}

// joins dir + name into buf with exactly one separator. libc-free, prx-safe.
static inline char *joinPath(char *buf, int bufSize, const char *dir, const char *name)
{
    int o = 0;
    while (dir[o] && o < bufSize - 1) { buf[o] = dir[o]; o++; }
    if (o > 0 && buf[o - 1] != '/' && o < bufSize - 1) buf[o++] = '/';
    for (int i = 0; name[i] && o < bufSize - 1; i++) buf[o++] = name[i];
    buf[o] = '\0';
    return buf;
}

// truncates path in-place to its parent. "/a/b/c" -> "/a/b", "/a/b/" -> "/a",
// "/" stays "/". no-op on empty paths.
static inline void toParentPath(char *path)
{
    int len = strLen(path);
    if (len <= 1) return;
    if (path[len - 1] == '/') len--;
    while (len > 1 && path[len - 1] != '/') len--;
    if (len <= 1) { path[0] = '/'; path[1] = '\0'; }
    else          { path[len] = '\0'; }
}

static inline const char *getExtension(const char *name)
{
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    return dot ? dot + 1 : NULL;
}

static inline int isDir(const char *path)
{
    CellFsStat st;
    if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) return 0;
    return (st.st_mode & CELL_FS_S_IFDIR) != 0;
}

// formats byte count as "1.23 MB" / "456 B" etc. buf must hold at least 16 bytes.
static inline void formatSize(uint64_t bytes, char *buf)
{
    static const uint64_t thresh[] = { 1073741824ULL, 1048576ULL, 1024ULL };
    static const char *units[]     = { " GB",         " MB",      " KB"  };
    int p = 0;
    for (int i = 0; i < 3; i++) {
        if (bytes >= thresh[i]) {
            p = intToDec((int)(bytes / thresh[i]), buf);
            uint64_t frac = (bytes % thresh[i]) * 100 / thresh[i];
            buf[p++] = '.';
            buf[p++] = '0' + (frac / 10) % 10;
            buf[p++] = '0' + frac % 10;
            const char *u = units[i];
            while (*u) buf[p++] = *u++;
            buf[p] = '\0';
            return;
        }
    }
    p = intToDec((int)bytes, buf);
    buf[p++] = ' '; buf[p++] = 'B'; buf[p] = '\0';
}

// reads up to cap-1 bytes into buf and NUL-terminates. returns bytes read, or -1.
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

static inline int fileExists(const char *path)
{
    CellFsStat st;
    return cellFsStat(path, &st) == CELL_FS_SUCCEEDED;
}

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

// creates a directory. returns 0 if created or already present.
static inline int makeDir(const char *path)
{
    int r = cellFsMkdir(path, CELL_FS_S_IFDIR | 0777);
    return (r == CELL_FS_SUCCEEDED || r == (int)CELL_FS_EEXIST) ? 0 : r;
}

// idempotent: returns 0 if the file did not exist.
static inline int deleteFile(const char *path)
{
    int r = cellFsUnlink(path);
    return (r == CELL_FS_SUCCEEDED || r == (int)CELL_FS_ENOENT) ? 0 : -1;
}

// recursively deletes path (file or dir). adds the size of every removed
// regular file into *bytesFreed (pass NULL to ignore). idempotent on ENOENT.
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

    char child[MAX_PATH_LEN];
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
