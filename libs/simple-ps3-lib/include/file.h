#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include "format.h"
#include <string.h>

#define MAX_PATH_LEN 512

// joins a directory path and filename into buf. returns buf.
static inline char *joinPath(char *buf, int bufSize, const char *dir, const char *name)
{
    int dlen = strlen(dir);
    if (dlen >= bufSize) dlen = bufSize - 1;
    memcpy(buf, dir, dlen);
    if (dlen > 0 && dir[dlen - 1] != '/') buf[dlen++] = '/';
    int nlen = strlen(name);
    if (dlen + nlen >= bufSize) nlen = bufSize - 1 - dlen;
    memcpy(buf + dlen, name, nlen);
    buf[dlen + nlen] = '\0';
    return buf;
}

// returns pointer to the file extension (after the last dot), or NULL if none
static inline const char *getExtension(const char *name)
{
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    return dot ? dot + 1 : NULL;
}

// returns 1 if path is a directory
static inline int isDir(const char *path)
{
    CellFsStat st;
    if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) return 0;
    return (st.st_mode & CELL_FS_S_IFDIR) != 0;
}

// mounts /dev_blind (writable mirror of /dev_flash). cobra/evilnat only.
// syscall 837, idempotent — second call returns error which is safe to ignore.
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

// formats byte count as human-readable size string (e.g. "1.23 MB")
// buf must be at least 16 bytes
static inline void formatSize(uint64_t bytes, char *buf)
{
    static const uint64_t thresh[] = { 1073741824ULL, 1048576ULL, 1024ULL };
    static const char *units[]     = { " GB",         " MB",      " KB"  };
    int p = 0;
    for (int i = 0; i < 3; i++) {
        if (bytes >= thresh[i]) {
            p = intToStr((int)(bytes / thresh[i]), buf);
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
    p = intToStr((int)bytes, buf);
    buf[p++] = ' '; buf[p++] = 'B'; buf[p] = '\0';
}

// Creates a directory.
static inline int makeDir(const char *path)
{
    int r = cellFsMkdir(path, CELL_FS_S_IFDIR | 0777);
    return (r == CELL_FS_SUCCEEDED || r == (int)CELL_FS_EEXIST) ? 0 : r;
}

// Reads entire file into a malloc'd buffer. Sets *outSize on success.
// Caller must free() the returned pointer. Returns NULL on failure.
static inline uint8_t *readFileAlloc(const char *path, uint32_t *outSize)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return NULL;
    CellFsStat st;
    if (cellFsFstat(fd, &st) != CELL_FS_SUCCEEDED) { cellFsClose(fd); return NULL; }
    uint32_t size = (uint32_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) { cellFsClose(fd); return NULL; }
    uint64_t totalRead = 0;
    while (totalRead < size) {
        uint64_t r;
        if (cellFsRead(fd, buf + totalRead, size - totalRead, &r) != CELL_FS_SUCCEEDED || r == 0) break;
        totalRead += r;
    }
    cellFsClose(fd);
    if (totalRead != size) { free(buf); return NULL; }
    *outSize = size;
    return buf;
}
