// file - progress-reporting, cancellable tree operations (declared in file.h;
// the plain allocation-free helpers live inline there). these mirror the plain
// copyTree/deleteTree but report bytes through onBytes(n) and bail when
// cancelled() returns non-zero; either callback may be NULL. prx-safe: only the
// VFS primitives and the inline path helpers, no libc/malloc. all paths route
// through the VFS, so these work the same on any backend (cellFs/NTFS/exFAT).
#include "file.h"

uint64_t measureTree(const char *path, int (*cancelled)(void))
{
   if (cancelled && cancelled()) return 0;

   VfsStat info;
   if (statPath(path, &info) != 0) return 0;
   if (!info.isDir) return info.size;

   VfsDir dir;
   if (openDir(path, &dir) != 0) return 0;

   uint64_t sum = 0;
   char name[256];
   char child[MAX_PATH_LEN];
   while (readDir(&dir, name, sizeof name, NULL) == 1) {
      if (cancelled && cancelled()) break;
      joinPath(child, MAX_PATH_LEN, path, name);
      sum += measureTree(child, cancelled);
   }
   closeDir(&dir);
   return sum;
}

static int copyFileProgress(const char *src, const char *dst, void *buf, int bufSize,
                            void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   VfsFile in;
   if (openFs(src, VFS_O_RDONLY, &in) != 0) return -1;
   VfsFile out;
   if (openFs(dst, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &out) != 0) {
      closeFs(&in);
      return -1;
   }
   int rc = 0;
   for (;;) {
      if (cancelled && cancelled()) { rc = 1; break; }
      int64_t got = readFs(&in, buf, (uint64_t)bufSize);
      if (got < 0) { rc = -1; break; }
      if (got == 0) break;
      if (writeFs(&out, buf, (uint64_t)got) != got) { rc = -1; break; }
      if (onBytes) onBytes((uint64_t)got);
   }
   closeFs(&in);
   closeFs(&out);
   return rc;
}

static int copyTreeRecursively(const char *src, const char *dst, void *buf, int bufSize,
                               void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   if (cancelled && cancelled()) return 1;

   VfsStat info;
   if (statPath(src, &info) != 0) return -1;
   if (!info.isDir) return copyFileProgress(src, dst, buf, bufSize, onBytes, cancelled);

   if (makeDir(dst) != 0) return -1;

   VfsDir dir;
   if (openDir(src, &dir) != 0) return -1;

   char name[256];
   char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
   int rc = 0;
   while (readDir(&dir, name, sizeof name, NULL) == 1) {
      if (cancelled && cancelled()) { rc = 1; break; }
      joinPath(childSrc, MAX_PATH_LEN, src, name);
      joinPath(childDst, MAX_PATH_LEN, dst, name);
      rc = copyTreeRecursively(childSrc, childDst, buf, bufSize, onBytes, cancelled);
      if (rc != 0) break;
   }
   closeDir(&dir);
   return rc;
}

int copyTreeProgress(const char *src, const char *dst, void *buf, int bufSize,
                     void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   int rc = copyTreeRecursively(src, dst, buf, bufSize, onBytes, cancelled);

   // one flush at the true batch boundary: makes every byte just written durable
   // and the next free-size read accurate. run even on error/cancel, since a
   // partial copy still left data on the volume. best-effort -- never fails the op.
   syncVfs(dst);
   return rc;
}

static int mergeTreeRecursively(const char *src, const char *dst, int replaceExisting,
                                void *buf, int bufSize,
                                void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   if (cancelled && cancelled()) return 1;

   VfsStat info;
   if (statPath(src, &info) != 0) return -1;

   if (!info.isDir) {
      // file leaf: on a collision, replace or keep the destination per the flag.
      if (!replaceExisting && fileExists(dst)) {
         if (onBytes) onBytes(info.size);  // still part of the total
         return 0;
      }
      return copyFileProgress(src, dst, buf, bufSize, onBytes, cancelled);
   }

   if (makeDir(dst) != 0) return -1;  // no-op when dst already exists

   VfsDir dir;
   if (openDir(src, &dir) != 0) return -1;

   char name[256];
   char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
   int rc = 0;
   while (readDir(&dir, name, sizeof name, NULL) == 1) {
      if (cancelled && cancelled()) { rc = 1; break; }
      joinPath(childSrc, MAX_PATH_LEN, src, name);
      joinPath(childDst, MAX_PATH_LEN, dst, name);
      rc = mergeTreeRecursively(childSrc, childDst, replaceExisting, buf, bufSize, onBytes, cancelled);
      if (rc != 0) break;
   }
   closeDir(&dir);
   return rc;
}

int mergeTreeProgress(const char *src, const char *dst, int replaceExisting,
                      void *buf, int bufSize,
                      void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   int rc = mergeTreeRecursively(src, dst, replaceExisting, buf, bufSize, onBytes, cancelled);

   syncVfs(dst);
   return rc;
}

int countTreeConflicts(const char *src, const char *dst, int cap)
{
   if (cap <= 0) return 0;

   VfsStat srcInfo;
   if (statPath(src, &srcInfo) != 0) return 0;

   VfsStat dstInfo;
   int dstExists = (statPath(dst, &dstInfo) == 0);

   if (!srcInfo.isDir)
      return dstExists ? 1 : 0;        // a file leaf conflicts if anything is at dst

   if (!dstExists) return 0;           // dir merging into nothing: all new
   if (!dstInfo.isDir) return 1;       // dir vs existing file: one clash

   VfsDir dir;
   if (openDir(src, &dir) != 0) return 0;

   char name[256];
   char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
   int count = 0;
   while (count < cap && readDir(&dir, name, sizeof name, NULL) == 1) {
      joinPath(childSrc, MAX_PATH_LEN, src, name);
      joinPath(childDst, MAX_PATH_LEN, dst, name);
      count += countTreeConflicts(childSrc, childDst, cap - count);
   }
   closeDir(&dir);
   return count;
}

static int deleteTreeRecursively(const char *path, void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   if (cancelled && cancelled()) return 1;

   VfsStat info;
   // not stattable: removeFilePath is idempotent (already gone -> 0) but still
   // reports a real failure (-1), so a delete never claims false success.
   if (statPath(path, &info) != 0) return removeFilePath(path);

   if (!info.isDir) {
      uint64_t size = info.size;
      if (deleteFile(path) < 0) return -1;
      if (onBytes) onBytes(size);
      return 0;
   }

   VfsDir dir;
   if (openDir(path, &dir) != 0) return -1;

   char name[256];
   char child[MAX_PATH_LEN];
   int rc = 0;
   while (readDir(&dir, name, sizeof name, NULL) == 1) {
      if (cancelled && cancelled()) { rc = 1; break; }
      joinPath(child, MAX_PATH_LEN, path, name);
      rc = deleteTreeRecursively(child, onBytes, cancelled);
      if (rc != 0) break;
   }
   closeDir(&dir);
   if (rc != 0) return rc;

   return removeDirPath(path) == 0 ? 0 : -1;
}

int deleteTreeProgress(const char *path, void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   int rc = deleteTreeRecursively(path, onBytes, cancelled);

   // flush the directory-entry removals and freed blocks to disk once, so the
   // volume can't be left mid-update and the freed space shows up immediately.
   syncVfs(path);
   return rc;
}
