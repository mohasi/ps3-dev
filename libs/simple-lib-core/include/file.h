#pragma once

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include "string-utilities.h"
#include "syscall.h"  // scCall trampolines, mountDevBlind (837), syncDevice (839)

#define MAX_PATH_LEN 512

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
    int len = getStrLen(path);
    if (len <= 1) return;
    if (path[len - 1] == '/') len--;
    while (len > 1 && path[len - 1] != '/') len--;
    if (len <= 1) { path[0] = '/'; path[1] = '\0'; }
    else          { path[len] = '\0'; }
}

// copies the parent of path into parent without mutating the input.
static inline void getParentPath(const char *path, char *parent, int cap)
{
    if (!path || !parent || cap <= 0) return;

    strCopy(parent, cap, path);
    toParentPath(parent);
}

// returns the final path component (the name) of path. for "/a/b/c" -> "c",
// for "/a/b/" -> "" (trailing slash), for "name" -> "name". points into path.
static inline const char *getBaseName(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

// copies the device mount root of an absolute path into out (size >= 16):
// "/dev_usb000/x/y" -> "/dev_usb000", "/dev_hdd0/..." -> "/dev_hdd0",
// "/dev_blind" -> "/dev_blind". a bare root or a path with no second '/' is
// copied whole. pass the result to syncDevice. returns out.
static inline char *deviceRootOf(const char *path, char *out, int outSize)
{
    int o = 0;
    if (path[0] == '/' && o < outSize - 1) {
        out[o++] = '/';
        for (int i = 1; path[i] && path[i] != '/' && o < outSize - 1; i++)
            out[o++] = path[i];
    }
    out[o] = '\0';
    return out;
}

// flushes the volume that path lives on (syncDevice on its device root). use
// after a write/unlink/rename so the change is durable and the free-size the
// XMB reports is accurate. for a genuine cross-volume operation (e.g. moveTree)
// sync each root separately -- this only touches the one path's volume.
static inline void syncPath(const char *path)
{
    char root[16];
    syncDevice(deviceRootOf(path, root, sizeof root));
}

static inline const char *getExtension(const char *name)
{
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    return dot ? dot + 1 : NULL;
}

// returns 1 if name is usable as a single path component: non-empty, shorter
// than MAX_PATH_LEN, not the "." or ".." aliases, and free of control bytes and
// the FAT/exFAT reserved characters (/ \ : * ? " < > |). use to vet names from
// untrusted sources (e.g. on-screen keyboard input) before a create or rename.
static inline int isValidFileName(const char *name)
{
    if (!name || name[0] == '\0') return 0;
    if (strEq(name, ".") || strEq(name, "..")) return 0;
    if (getStrLen(name) >= MAX_PATH_LEN) return 0;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        if (*c < 0x20) return 0;  // control characters
        if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' ||
            *c == '"' || *c == '<'  || *c == '>' || *c == '|')
            return 0;
    }
    return 1;
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

// like formatSize, but appends a trailing '+' when the byte count is only a
// lower bound (e.g. a folder walk that hit its time budget). buf must hold >= 16.
static inline void formatSizeApprox(uint64_t bytes, int approx, char *buf)
{
    formatSize(bytes, buf);
    if (!approx) return;
    int n = getStrLen(buf);
    buf[n]     = '+';
    buf[n + 1] = '\0';
}

// reads up to cap-1 bytes into buf and NUL-terminates. returns bytes read, or -1.
static inline int readFile(const char *path, char *buf, int cap)
{
    int descriptor;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &descriptor, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
    uint64_t bytesRead = 0;
    int r = cellFsRead(descriptor, buf, (uint64_t)(cap - 1), &bytesRead);
    cellFsClose(descriptor);
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
    int descriptor;
    if (cellFsOpen(path, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &descriptor, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
    uint64_t written = 0;
    int r = cellFsWrite(descriptor, data, len, &written);
    cellFsClose(descriptor);
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

    int descriptor;
    if (cellFsOpendir(path, &descriptor) != CELL_FS_SUCCEEDED) return -1;

    char child[MAX_PATH_LEN];
    CellFsDirent ent;
    uint64_t n;
    while (cellFsReaddir(descriptor, &ent, &n) == CELL_FS_SUCCEEDED && n > 0) {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
            (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) continue;
        int len = 0;
        while (path[len] && len < (int)sizeof child - 2) { child[len] = path[len]; len++; }
        if (len > 0 && child[len - 1] != '/' && len < (int)sizeof child - 1) child[len++] = '/';
        for (int i = 0; ent.d_name[i] && len < (int)sizeof child - 1; i++) child[len++] = ent.d_name[i];
        child[len] = '\0';
        if (deleteTree(child, bytesFreed) < 0) { cellFsClosedir(descriptor); return -1; }
    }
    cellFsClosedir(descriptor);

    return cellFsRmdir(path) == CELL_FS_SUCCEEDED ? 0 : -1;
}

// copies a single regular file src -> dst (created/truncated). buf is caller
// scratch of bufSize bytes (e.g. 64 KB) - kept caller-provided so this stays
// allocation-free and light on stack, safe to use from prx contexts.
// returns 0 on success, -1 on failure.
static inline int copyFile(const char *src, const char *dst, void *buf, int bufSize)
{
    int in;
    if (cellFsOpen(src, CELL_FS_O_RDONLY, &in, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
    int out;
    if (cellFsOpen(dst, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC, &out, NULL, 0) != CELL_FS_SUCCEEDED) {
        cellFsClose(in);
        return -1;
    }
    int rc = 0;
    for (;;) {
        uint64_t got = 0;
        if (cellFsRead(in, buf, (uint64_t)bufSize, &got) != CELL_FS_SUCCEEDED) { rc = -1; break; }
        if (got == 0) break;
        uint64_t put = 0;
        if (cellFsWrite(out, buf, got, &put) != CELL_FS_SUCCEEDED || put != got) { rc = -1; break; }
    }
    cellFsClose(in);
    cellFsClose(out);
    return rc;
}

// recursively copies src (file or dir) to dst. buf/bufSize is caller scratch
// for the file payload (see copyFile). returns 0 on success, -1 on failure.
static inline int copyTree(const char *src, const char *dst, void *buf, int bufSize)
{
    CellFsStat st;
    if (cellFsStat(src, &st) != CELL_FS_SUCCEEDED) return -1;
    if (!(st.st_mode & CELL_FS_S_IFDIR)) return copyFile(src, dst, buf, bufSize);

    if (makeDir(dst) != 0) return -1;

    int descriptor;
    if (cellFsOpendir(src, &descriptor) != CELL_FS_SUCCEEDED) return -1;

    char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
    CellFsDirent ent;
    uint64_t n;
    int rc = 0;
    while (cellFsReaddir(descriptor, &ent, &n) == CELL_FS_SUCCEEDED && n > 0) {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
            (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) continue;
        joinPath(childSrc, MAX_PATH_LEN, src, ent.d_name);
        joinPath(childDst, MAX_PATH_LEN, dst, ent.d_name);
        if (copyTree(childSrc, childDst, buf, bufSize) < 0) { rc = -1; break; }
    }
    cellFsClosedir(descriptor);
    return rc;
}

// moves src -> dst: a same-volume rename when possible, otherwise a recursive
// copy followed by deleting the source (cross-volume). does not pre-clear dst,
// so an existing destination directory will make the rename fail; callers that
// want overwrite semantics should deleteTree(dst) first. buf/bufSize is scratch
// for the cross-volume copy. returns 0 on success, -1 on failure.
static inline int moveTree(const char *src, const char *dst, void *buf, int bufSize)
{
    int rc;
    if (cellFsRename(src, dst) == CELL_FS_SUCCEEDED) {
        rc = 0;
    } else {
        rc = copyTree(src, dst, buf, bufSize);   // may leave partial data on dst
        if (rc == 0) rc = deleteTree(src, NULL);
    }

    // flush both ends, regardless of rc: the destination gained entries/data
    // (or a failed cross-volume copy left a partial tree there) and the source
    // lost them. rename re-links within one volume; a cross-volume move touches
    // two, so sync the source root too when it differs.
    char srcRoot[16], dstRoot[16];
    deviceRootOf(src, srcRoot, sizeof srcRoot);
    deviceRootOf(dst, dstRoot, sizeof dstRoot);
    syncDevice(dstRoot);
    if (!strEq(srcRoot, dstRoot)) syncDevice(srcRoot);
    return rc;
}

// progress-reporting, cancellable cousins of the operations above (defined in
// file.c). they call onBytes(n) as regular-file bytes are read/deleted and poll
// cancelled() between entries/chunks; either callback may be NULL. allocation-
// free and prx-safe like the inlines above. return values:
//   measureTree        - sum of regular-file bytes under path (partial on cancel).
//   copyTreeProgress   - 0 ok, -1 error, 1 cancelled. reports bytes per chunk.
//   deleteTreeProgress - 0 ok, -1 error, 1 cancelled. reports bytes per file.
// buf/bufSize is caller scratch for the copy payload (e.g. a 64 KB buffer).
uint64_t measureTree(const char *path, int (*cancelled)(void));
int      copyTreeProgress(const char *src, const char *dst, void *buf, int bufSize,
                          void (*onBytes)(uint64_t), int (*cancelled)(void));
int      deleteTreeProgress(const char *path,
                            void (*onBytes)(uint64_t), int (*cancelled)(void));

// merges src into dst, creating dst if absent and descending into it if it is an
// existing directory (makeDir is a no-op on an existing folder). for regular-file
// leaves that already exist at dst, replaceExisting != 0 overwrites them while
// replaceExisting == 0 leaves the destination file untouched - a skipped file's
// bytes are still reported through onBytes so a progress total stays consistent.
// like copyTreeProgress: 0 ok, -1 error, 1 cancelled. buf/bufSize is copy scratch.
int      mergeTreeProgress(const char *src, const char *dst, int replaceExisting,
                           void *buf, int bufSize,
                           void (*onBytes)(uint64_t), int (*cancelled)(void));

// counts the regular-file leaves of src that would land on top of an existing
// entry if src were merged into dst (i.e. how many files a merge would replace).
// counting stops once cap is reached, so pass a small cap (e.g. 2) when only the
// "none / one / many" distinction matters. a directory whose dst counterpart does
// not exist contributes no conflicts (the whole subtree is new).
int      countTreeConflicts(const char *src, const char *dst, int cap);
