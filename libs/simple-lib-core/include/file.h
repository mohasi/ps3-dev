#pragma once

#include <stdint.h>
#include <stddef.h>   // NULL
#include "string-utilities.h"
#include "vfs.h"      // every fs helper here routes through the VFS (cellFs/NTFS/exFAT)

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
   else          { path[len - 1] = '\0'; }   // cut at the separating '/', not after it
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
// copied whole. returns out.
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

// flushes the volume that path lives on so a write/unlink/rename is durable and
// the free-size the XMB reports is accurate. backend-aware (no-op off cellFs).
static inline void syncPath(const char *path)
{
   syncVfs(path);
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
   VfsStat info;
   return statPath(path, &info) == 0 && info.isDir;
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
   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return -1;
   int64_t bytesRead = readFs(&file, buf, (uint64_t)(cap - 1));
   closeFs(&file);
   if (bytesRead <= 0) return -1;
   buf[bytesRead] = '\0';
   return (int)bytesRead;
}

static inline int fileExists(const char *path)
{
   VfsStat info;
   return statPath(path, &info) == 0;
}

static inline int writeFile(const char *path, const char *data, uint64_t len)
{
   VfsFile file;
   if (openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file) != 0) return -1;
   int64_t written = writeFs(&file, data, len);
   closeFs(&file);
   return (written == (int64_t)len) ? 0 : -1;
}

// creates a directory. returns 0 if created or already present.
static inline int makeDir(const char *path)
{
   return makeDirPath(path);
}

// idempotent: returns 0 if the file did not exist.
static inline int deleteFile(const char *path)
{
   return removeFilePath(path);
}

// recursively deletes path (file or dir). adds the size of every removed
// regular file into *bytesFreed (pass NULL to ignore). idempotent when absent.
static inline int deleteTree(const char *path, uint64_t *bytesFreed)
{
   VfsStat info;
   // not stattable: removeFilePath is idempotent (already gone -> 0) but still
   // reports a real failure (-1), so a delete never claims false success.
   if (statPath(path, &info) != 0) return removeFilePath(path);

   if (!info.isDir) {
      if (bytesFreed) *bytesFreed += info.size;
      return deleteFile(path);
   }

   VfsDir dir;
   if (openDir(path, &dir) != 0) return -1;

   char name[256], child[MAX_PATH_LEN];
   while (readDir(&dir, name, sizeof name, NULL) == 1) {
      joinPath(child, MAX_PATH_LEN, path, name);
      if (deleteTree(child, bytesFreed) < 0) { closeDir(&dir); return -1; }
   }
   closeDir(&dir);

   return removeDirPath(path) == 0 ? 0 : -1;
}

// copies a single regular file src -> dst (created/truncated). buf is caller
// scratch of bufSize bytes (e.g. 64 KB) - kept caller-provided so this stays
// allocation-free and light on stack, safe to use from prx contexts.
// returns 0 on success, -1 on failure.
static inline int copyFile(const char *src, const char *dst, void *buf, int bufSize)
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
      int64_t got = readFs(&in, buf, (uint64_t)bufSize);
      if (got < 0) { rc = -1; break; }
      if (got == 0) break;
      if (writeFs(&out, buf, (uint64_t)got) != got) { rc = -1; break; }
   }
   closeFs(&in);
   closeFs(&out);
   return rc;
}

// recursively copies src (file or dir) to dst. buf/bufSize is caller scratch
// for the file payload (see copyFile). returns 0 on success, -1 on failure.
static inline int copyTree(const char *src, const char *dst, void *buf, int bufSize)
{
   VfsStat info;
   if (statPath(src, &info) != 0) return -1;
   if (!info.isDir) return copyFile(src, dst, buf, bufSize);

   if (makeDir(dst) != 0) return -1;

   VfsDir dir;
   if (openDir(src, &dir) != 0) return -1;

   char name[256], childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
   int rc = 0;
   while (readDir(&dir, name, sizeof name, NULL) == 1) {
      joinPath(childSrc, MAX_PATH_LEN, src, name);
      joinPath(childDst, MAX_PATH_LEN, dst, name);
      if (copyTree(childSrc, childDst, buf, bufSize) < 0) { rc = -1; break; }
   }
   closeDir(&dir);
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
   if (renamePath(src, dst) == 0) {
      rc = 0;
   } else {
      rc = copyTree(src, dst, buf, bufSize);   // may leave partial data on dst
      if (rc == 0) rc = deleteTree(src, NULL);
   }

   // flush both ends, regardless of rc: the destination gained entries/data
   // (or a failed cross-volume copy left a partial tree there) and the source
   // lost them. a same-volume rename touches one root; a cross-volume move
   // touches two, so sync the source root too when it differs.
   char srcRoot[16], dstRoot[16];
   deviceRootOf(src, srcRoot, sizeof srcRoot);
   deviceRootOf(dst, dstRoot, sizeof dstRoot);
   syncVfs(dst);
   if (!strEq(srcRoot, dstRoot)) syncVfs(src);
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
