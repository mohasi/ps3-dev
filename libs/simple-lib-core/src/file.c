// file - progress-reporting, cancellable tree operations (declared in file.h;
// the plain allocation-free helpers live inline there). these mirror the plain
// copyTree/deleteTree but report bytes through onBytes(n) and bail when
// cancelled() returns non-zero; either callback may be NULL. prx-safe: only
// cellFs calls and the inline path helpers, no libc/malloc.
#include "file.h"

uint64_t measureTree(const char *path, int (*cancelled)(void))
{
    if (cancelled && cancelled()) return 0;

    CellFsStat st;
    if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) return 0;
    if (!(st.st_mode & CELL_FS_S_IFDIR)) return (uint64_t)st.st_size;

    int descriptor;
    if (cellFsOpendir(path, &descriptor) != CELL_FS_SUCCEEDED) return 0;

    uint64_t sum = 0;
    char child[MAX_PATH_LEN];
    CellFsDirent ent;
    uint64_t n;
    while (cellFsReaddir(descriptor, &ent, &n) == CELL_FS_SUCCEEDED && n > 0) {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
            (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) continue;
        if (cancelled && cancelled()) break;
        joinPath(child, MAX_PATH_LEN, path, ent.d_name);
        sum += measureTree(child, cancelled);
    }
    cellFsClosedir(descriptor);
    return sum;
}

static int copyFileProgress(const char *src, const char *dst, void *buf, int bufSize,
                            void (*onBytes)(uint64_t), int (*cancelled)(void))
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
        if (cancelled && cancelled()) { rc = 1; break; }
        uint64_t got = 0;
        if (cellFsRead(in, buf, (uint64_t)bufSize, &got) != CELL_FS_SUCCEEDED) { rc = -1; break; }
        if (got == 0) break;
        uint64_t put = 0;
        if (cellFsWrite(out, buf, got, &put) != CELL_FS_SUCCEEDED || put != got) { rc = -1; break; }
        if (onBytes) onBytes(got);
    }
    cellFsClose(in);
    cellFsClose(out);
    return rc;
}

static int copyTreeRecursively(const char *src, const char *dst, void *buf, int bufSize,
                               void (*onBytes)(uint64_t), int (*cancelled)(void))
{
    if (cancelled && cancelled()) return 1;

    CellFsStat st;
    if (cellFsStat(src, &st) != CELL_FS_SUCCEEDED) return -1;
    if (!(st.st_mode & CELL_FS_S_IFDIR)) return copyFileProgress(src, dst, buf, bufSize, onBytes, cancelled);

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
        if (cancelled && cancelled()) { rc = 1; break; }
        joinPath(childSrc, MAX_PATH_LEN, src, ent.d_name);
        joinPath(childDst, MAX_PATH_LEN, dst, ent.d_name);
        rc = copyTreeRecursively(childSrc, childDst, buf, bufSize, onBytes, cancelled);
        if (rc != 0) break;
    }
    cellFsClosedir(descriptor);
    return rc;
}

int copyTreeProgress(const char *src, const char *dst, void *buf, int bufSize,
                     void (*onBytes)(uint64_t), int (*cancelled)(void))
{
    int rc = copyTreeRecursively(src, dst, buf, bufSize, onBytes, cancelled);

    // one flush at the true batch boundary: makes every byte just written durable
    // and the next free-size read accurate. run even on error/cancel, since a
    // partial copy still left data on the volume. best-effort -- never fails the op.
    syncPath(dst);
    return rc;
}

static int mergeTreeRecursively(const char *src, const char *dst, int replaceExisting,
                                void *buf, int bufSize,
                                void (*onBytes)(uint64_t), int (*cancelled)(void))
{
    if (cancelled && cancelled()) return 1;

    CellFsStat st;
    if (cellFsStat(src, &st) != CELL_FS_SUCCEEDED) return -1;

    if (!(st.st_mode & CELL_FS_S_IFDIR)) {
        // file leaf: on a collision, replace or keep the destination per the flag.
        if (!replaceExisting && fileExists(dst)) {
            if (onBytes) onBytes((uint64_t)st.st_size);  // still part of the total
            return 0;
        }
        return copyFileProgress(src, dst, buf, bufSize, onBytes, cancelled);
    }

    if (makeDir(dst) != 0) return -1;  // no-op when dst already exists

    int descriptor;
    if (cellFsOpendir(src, &descriptor) != CELL_FS_SUCCEEDED) return -1;

    char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
    CellFsDirent ent;
    uint64_t n;
    int rc = 0;
    while (cellFsReaddir(descriptor, &ent, &n) == CELL_FS_SUCCEEDED && n > 0) {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
            (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) continue;
        if (cancelled && cancelled()) { rc = 1; break; }
        joinPath(childSrc, MAX_PATH_LEN, src, ent.d_name);
        joinPath(childDst, MAX_PATH_LEN, dst, ent.d_name);
        rc = mergeTreeRecursively(childSrc, childDst, replaceExisting, buf, bufSize, onBytes, cancelled);
        if (rc != 0) break;
    }
    cellFsClosedir(descriptor);
    return rc;
}

int mergeTreeProgress(const char *src, const char *dst, int replaceExisting,
                      void *buf, int bufSize,
                      void (*onBytes)(uint64_t), int (*cancelled)(void))
{
    int rc = mergeTreeRecursively(src, dst, replaceExisting, buf, bufSize, onBytes, cancelled);

    syncPath(dst);
    return rc;
}

int countTreeConflicts(const char *src, const char *dst, int cap)
{
    if (cap <= 0) return 0;

    CellFsStat ss;
    if (cellFsStat(src, &ss) != CELL_FS_SUCCEEDED) return 0;

    CellFsStat ds;
    int dstExists = (cellFsStat(dst, &ds) == CELL_FS_SUCCEEDED);

    if (!(ss.st_mode & CELL_FS_S_IFDIR))
        return dstExists ? 1 : 0;       // a file leaf conflicts if anything is at dst

    if (!dstExists) return 0;           // dir merging into nothing: all new
    if (!(ds.st_mode & CELL_FS_S_IFDIR)) return 1;  // dir vs existing file: one clash

    int descriptor;
    if (cellFsOpendir(src, &descriptor) != CELL_FS_SUCCEEDED) return 0;

    char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
    CellFsDirent ent;
    uint64_t n;
    int count = 0;
    while (count < cap && cellFsReaddir(descriptor, &ent, &n) == CELL_FS_SUCCEEDED && n > 0) {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
            (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) continue;
        joinPath(childSrc, MAX_PATH_LEN, src, ent.d_name);
        joinPath(childDst, MAX_PATH_LEN, dst, ent.d_name);
        count += countTreeConflicts(childSrc, childDst, cap - count);
    }
    cellFsClosedir(descriptor);
    return count;
}

static int deleteTreeRecursively(const char *path, void (*onBytes)(uint64_t), int (*cancelled)(void))
{
    if (cancelled && cancelled()) return 1;

    CellFsStat st;
    int r = cellFsStat(path, &st);
    if (r == (int)CELL_FS_ENOENT) return 0;
    if (r != CELL_FS_SUCCEEDED)   return -1;

    if (!(st.st_mode & CELL_FS_S_IFDIR)) {
        uint64_t sz = (uint64_t)st.st_size;
        if (deleteFile(path) < 0) return -1;
        if (onBytes) onBytes(sz);
        return 0;
    }

    int descriptor;
    if (cellFsOpendir(path, &descriptor) != CELL_FS_SUCCEEDED) return -1;

    char child[MAX_PATH_LEN];
    CellFsDirent ent;
    uint64_t n;
    int rc = 0;
    while (cellFsReaddir(descriptor, &ent, &n) == CELL_FS_SUCCEEDED && n > 0) {
        if (ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
            (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) continue;
        if (cancelled && cancelled()) { rc = 1; break; }
        joinPath(child, MAX_PATH_LEN, path, ent.d_name);
        rc = deleteTreeRecursively(child, onBytes, cancelled);
        if (rc != 0) break;
    }
    cellFsClosedir(descriptor);
    if (rc != 0) return rc;

    return cellFsRmdir(path) == CELL_FS_SUCCEEDED ? 0 : -1;
}

int deleteTreeProgress(const char *path, void (*onBytes)(uint64_t), int (*cancelled)(void))
{
    int rc = deleteTreeRecursively(path, onBytes, cancelled);

    // flush the directory-entry removals and freed blocks to disk once, so the
    // volume can't be left mid-update and the freed space shows up immediately.
    syncPath(path);
    return rc;
}
