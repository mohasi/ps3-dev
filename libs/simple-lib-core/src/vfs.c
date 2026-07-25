//
// vfs.c - filesystem abstraction: the path-scheme router only. It owns the mount
// registry and the public dispatch wrappers, and names no concrete backend's
// internals - every backend is its own file behind the VfsOps vtable:
//   cellfs.c - HDD + kernel-mounted FAT32 USB + /dev_flash. the default route.
//   exfat.c  - hand-written exFAT. registers a probe/release pair at runtime.
//   ntfs.c   - hand-written NTFS. registers the same way, no router change.
//
// The bringup that actually names those backends (initVfs/shutdownVfs + the USB
// hotplug poll thread) lives in vfs-init.c, deliberately a separate translation
// unit: this toolchain's linker can't strip unreachable code, so a consumer that
// only routes paths (openDir on /dev_hdd0) pulls this object alone and never the
// exFAT/NTFS drivers. See vfs-internal.h.
//
// resolvePath peels the first path segment, matches it against the virtual-mount
// registry, and rewrites the path to the backend's native form ("/ntfs0/x" ->
// "ntfs0:/x"). unmatched paths are cellFs and pass through unchanged, so
// HDD/FAT32 behaviour is identical to before the VFS existed.

#include "vfs.h"

#include <stdint.h>
#include "string-utilities.h"   // strCopy, strEq
#include "syscall.h"            // syncDevice
#include "cellfs.h"             // CELLFS_OPS / ROOT_OPS - the default backend (its own file now)
#include "thread.h"             // sys_lwmutex helpers - serialize the mount registry
#include "vfs-internal.h"       // lock primitives shared with vfs-init.c

#define VFS_MAX_MOUNTS 24       // ntfs0..7 + exfat0..7 + ext0..7

// VfsOps is the public vtable from vfs.h. The default cellFs vtable lives in
// cellfs.c (CELLFS_OPS / ROOT_OPS); NTFS/exFAT register theirs at runtime.

// virtual mounts only (NTFS/exFAT/ext). cellFs devices are the default route and
// are real children of "/", so they are never registered here.
typedef struct {
   char                 segment[32];   // root segment, e.g. "ntfs0"
   char                 native[40];    // native prefix, e.g. "ntfs0:"
   char                 label[64];
   VfsScheme            scheme;
   uint32_t             flags;     // backend-declared capabilities (VFS_MOUNT_*)
   const VfsOps *backend;
   int                  present;
} MountEntry;

static MountEntry mounts[VFS_MAX_MOUNTS];
static int        mountCount;

// Serializes the mount registry. The hotplug poll thread (vfs-init.c) mutates mounts[] via
// addVfsMount/removeVfsMount on a different thread than the readers (findMount/resolvePath/
// listMounts), so every touch of mounts[]/mountCount takes this lock. Lock order is always
// exfatLock -> mountsLock (backends hold exfatLock when they publish/withdraw); readers take
// mountsLock alone, so there is no reverse path and no deadlock.
static sys_lwmutex_t mountsLock;
static int           mountsLockReady;

// Bootstrap-safe registry locking, shared with vfs-init.c via vfs-internal.h. The lock is created
// by ensureMountsLock (from initVfs), but the VFS must be callable before then (e.g. the logger
// writes to /dev_hdd0 via openFs during early startup). Until the lock exists, mountCount is 0 and
// nothing mutates the registry, so reading it without the lock is safe; these become real
// lock/unlock once initVfs has run.
void ensureMountsLock(void) { if (!mountsLockReady) { createLock(&mountsLock); mountsLockReady = 1; } }
void lockMounts(void)       { if (mountsLockReady) lock(&mountsLock); }
void unlockMounts(void)     { if (mountsLockReady) unlock(&mountsLock); }

void clearMounts(void)
{
   lockMounts();
   mountCount = 0;
   unlockMounts();
}

// section: path routing helpers

// copies the first '/'-segment into segment and returns the remainder
// ("/ntfs0/x" -> segment "ntfs0", returns "/x"; "/" -> segment "", returns "").
static const char *splitFirstSegment(const char *path, char *segment, int capacity)
{
   int length = 0;
   const char *cursor = path;
   if (*cursor == '/') cursor++;
   while (*cursor && *cursor != '/' && length < capacity - 1) segment[length++] = *cursor++;
   segment[length] = '\0';
   return cursor;
}

// caller holds mountsLock.
static MountEntry *findMount(const char *segment)
{
   if (segment[0] == '\0') return NULL;
   for (int i = 0; i < mountCount; i++)
      if (mounts[i].present && strEq(mounts[i].segment, segment)) return &mounts[i];
   return NULL;
}

static int isRootPath(const char *path)
{
   return path[0] == '/' && path[1] == '\0';
}

// a top-level device root like "/dev_hdd0", "/dev_flash" or a mounted USB: one
// path segment, no inner slash. deleting one of these would wipe an entire
// device (and /dev_flash would brick the console), so the delete flow refuses
// them outright - see deleteTree / deleteTreeProgress.
int isDeviceRoot(const char *path)
{
   if (path[0] != '/' || path[1] == '\0') return 0;   // not absolute, or "/" itself
   for (const char *c = path + 1; *c; c++)
      if (*c == '/') return 0;                          // has a child segment
   return 1;
}

// Backend support for the synthetic "/" reader in cellfs.c: hand it the next
// present virtual mount. The registry and its lock live here, so the cellFs
// backend asks rather than reaching in. advances *cursor; 1 if a name was
// written, 0 when exhausted.
int getNextRootMount(int *cursor, char *nameOut, int nameCapacity)
{
   lockMounts();
   while (*cursor < mountCount) {
      MountEntry *mount = &mounts[(*cursor)++];
      if (!mount->present) continue;
      strCopy(nameOut, nameCapacity, mount->segment);
      unlockMounts();
      return 1;
   }
   unlockMounts();
   return 0;
}

// section: path resolution

// resolves a consumer path to its backend and native form. cellFs paths are used verbatim (no
// copy); virtual-mount paths are rewritten into buffer. returns NULL if the rewrite would overflow
// buffer - truncating it would silently target the wrong file, so the caller must fail instead.
// reads the mount registry under mountsLock so a concurrent hotplug can't tear mount->native.
static const VfsOps *resolvePath(const char *path, char *buffer, int capacity, const char **native)
{
   char segment[32];
   const char *rest = splitFirstSegment(path, segment, sizeof segment);

   lockMounts();
   MountEntry *mount = findMount(segment);
   if (!mount) { unlockMounts(); *native = path; return &CELLFS_OPS; }

   // native = prefix + '/' + tail, exactly one '/' joining them ("/ntfs0/x" -> "ntfs0:/x")
   int length = 0;
   const char *prefix = mount->native;
   while (prefix[length] && length < capacity - 1) { buffer[length] = prefix[length]; length++; }
   int overflow = (prefix[length] != '\0');
   if (length < capacity - 1) buffer[length++] = '/';
   else overflow = 1;   // no room left for the joining '/': a prefix-only native would mis-target
   const char *tail = (rest[0] == '/') ? rest + 1 : rest;
   while (*tail && length < capacity - 1) buffer[length++] = *tail++;
   buffer[length] = '\0';
   const VfsOps *backend = mount->backend;
   unlockMounts();

   if (overflow || *tail) { *native = path; return NULL; }   // truncated: refuse rather than mis-target
   *native = buffer;
   return backend;
}

// section: mount registry

// publishes a mounted volume. reuses a withdrawn slot when one is free so repeated
// hotplug cycles can't exhaust the table. returns 0, or -1 if the table is full.
int addVfsMount(const char *segment, const char *native, const char *label, VfsScheme scheme, uint32_t flags, const VfsOps *ops)
{
   lockMounts();
   MountEntry *mount = NULL;
   for (int i = 0; i < mountCount; i++)
      if (!mounts[i].present) { mount = &mounts[i]; break; }
   if (!mount) {
      if (mountCount >= VFS_MAX_MOUNTS) { unlockMounts(); return -1; }
      mount = &mounts[mountCount++];
   }
   strCopy(mount->segment, sizeof mount->segment, segment);
   strCopy(mount->native,  sizeof mount->native,  native);
   strCopy(mount->label,   sizeof mount->label,   label && label[0] ? label : segment);
   mount->scheme  = scheme;
   mount->flags   = flags;
   mount->backend = ops;
   mount->present = 1;   // publish last: a reader sees a fully-written entry or none
   unlockMounts();
   return 0;
}

// backend-declared capability query: does path live on a mount that flagged VFS_MOUNT_REMOTE?
// cellFs (unregistered) paths are local, so they answer 0.
int isRemoteVolume(const char *path)
{
   char segment[32];
   splitFirstSegment(path, segment, sizeof segment);
   lockMounts();
   MountEntry *mount = findMount(segment);
   int remote = mount ? (mount->flags & VFS_MOUNT_REMOTE) != 0 : 0;
   unlockMounts();
   return remote;
}

void removeVfsMount(const char *segment)
{
   lockMounts();
   MountEntry *mount = findMount(segment);
   if (mount) mount->present = 0;
   unlockMounts();
}

// section: public api

int listMounts(VfsMount *outMounts, int capacity)
{
   int count = 0;
   lockMounts();
   for (int i = 0; i < mountCount && count < capacity; i++) {
      if (!mounts[i].present) continue;
      strCopy(outMounts[count].segment, sizeof outMounts[count].segment, mounts[i].segment);
      strCopy(outMounts[count].label,   sizeof outMounts[count].label,   mounts[i].label);
      outMounts[count].scheme     = mounts[i].scheme;
      outMounts[count].freeBytes  = 0;
      outMounts[count].totalBytes = 0;
      count++;
   }
   unlockMounts();
   return count;
}

VfsScheme getScheme(const char *path)
{
   char segment[32];
   splitFirstSegment(path, segment, sizeof segment);
   lockMounts();
   MountEntry *mount = findMount(segment);
   VfsScheme scheme = mount ? mount->scheme : VFS_SCHEME_CELLFS;
   unlockMounts();
   return scheme;
}

int statPath(const char *path, VfsStat *outStat)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
   if (!backend) return -1;   // path too long to route safely
   return backend->stat(native, outStat);
}

int renamePath(const char *oldPath, const char *newPath)
{
   char fromBuffer[MAX_PATH_LEN], toBuffer[MAX_PATH_LEN];
   const char *from, *to;
   const VfsOps *fromBackend = resolvePath(oldPath, fromBuffer, sizeof fromBuffer, &from);
   const VfsOps *toBackend   = resolvePath(newPath, toBuffer,   sizeof toBuffer,   &to);
   if (!fromBackend || fromBackend != toBackend) return -1;   // too long, or cross-volume (caller uses moveTree)
   return fromBackend->rename(from, to);
}

int makeDirPath(const char *path)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
   if (!backend) return -1;
   return backend->mkdir(native);
}

int removeFilePath(const char *path)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
   if (!backend) return -1;
   return backend->rmfile(native);
}

int removeDirPath(const char *path)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
   if (!backend) return -1;
   return backend->rmdir(native);
}

void syncVfs(const char *path)
{
   if (getScheme(path) != VFS_SCHEME_CELLFS) return;   // userland backends flush per-file (fsync)
   char root[34];   // '/' + up to a 31-char mount segment + NUL (see path.h deviceRootOf)
   int length = 0;
   if (path[0] == '/') {
      root[length++] = '/';
      for (int i = 1; path[i] && path[i] != '/' && length < (int)sizeof root - 1; i++) root[length++] = path[i];
   }
   root[length] = '\0';
   syncDevice(root);
}

int getFreeSpace(const char *path, uint64_t *freeBytes, uint64_t *totalBytes)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
   if (!backend) return -1;
   return backend->getfree(native, freeBytes, totalBytes);
}

int openDir(const char *path, VfsDir *dir)
{
   dir->descriptor   = -1;
   dir->nativeHandle = 0;
   if (isRootPath(path)) {
      dir->backend = &ROOT_OPS;
      return ROOT_OPS.opendir(NULL, dir);
   }
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
   if (!backend) return -1;   // path too long to route safely
   dir->backend = backend;
   return backend->opendir(native, dir);
}

int readDir(VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut)
{
   return ((const VfsOps *)dir->backend)->readdir(dir, nameOut, nameCapacity, typeOut);
}

void closeDir(VfsDir *dir)
{
   ((const VfsOps *)dir->backend)->closedir(dir);
}

int openFs(const char *path, int flags, VfsFile *file)
{
   file->descriptor = -1;
   file->abandoned  = 0;
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
   if (!backend) return -1;   // path too long to route safely
   file->backend = backend;
   return backend->open(native, flags, file);
}

int64_t readFs(VfsFile *file, void *buffer, uint64_t length)
{
   return ((const VfsOps *)file->backend)->read(file, buffer, length);
}

int64_t writeFs(VfsFile *file, const void *buffer, uint64_t length)
{
   return ((const VfsOps *)file->backend)->write(file, buffer, length);
}

int64_t seekFs(VfsFile *file, int64_t offset, int whence)
{
   return ((const VfsOps *)file->backend)->seek(file, offset, whence);
}

int fsyncFs(VfsFile *file)
{
   return ((const VfsOps *)file->backend)->fsync(file);
}

int closeFs(VfsFile *file)
{
   return ((const VfsOps *)file->backend)->close(file);
}

// ============================================================================
// section: cross-backend file & tree operations
//
// Higher-level actions composed from the primitives above. They route entirely
// through the public VFS API (open/read/dir/stat), so they work identically on
// every backend and contain no backend specifics. The plain operations are thin
// wrappers over the cancellable/progress ones so each tree-walk algorithm exists
// once (DRY). prx-safe: only VFS primitives + the inline path helpers, no libc.
// ============================================================================

// ---- single-shot metadata / file helpers -----------------------------------

int isDir(const char *path)
{
   VfsStat info;
   return statPath(path, &info) == 0 && info.isDir;
}

int fileExists(const char *path)
{
   VfsStat info;
   return statPath(path, &info) == 0;
}

int makeDir(const char *path)
{
   return makeDirPath(path);
}

// idempotent: returns 0 if the file did not exist.
int deleteFile(const char *path)
{
   return removeFilePath(path);
}

void syncPath(const char *path)
{
   syncVfs(path);
}

// reads up to cap-1 bytes into buf and NUL-terminates. returns bytes read
// (0 for a legitimately empty file, with buf[0] == '\0'), or -1 on error.
int readFile(const char *path, char *buf, int cap)
{
   if (!buf || cap < 1) return -1;   // cap<1 would promote cap-1 to a huge unsigned read
   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return -1;
   int64_t bytesRead = readFs(&file, buf, (uint64_t)(cap - 1));
   closeFs(&file);
   if (bytesRead < 0) return -1;     // a 0-byte (empty) file is success, not failure
   buf[bytesRead] = '\0';
   return (int)bytesRead;
}

int writeFile(const char *path, const char *data, uint64_t len)
{
   VfsFile file;
   if (openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file) != 0) return -1;
   int64_t written = writeFs(&file, data, len);
   // closeFs surfaces a commit error deferred to close (vfs.h): a backend can
   // ACK every writeFs yet still fail to flush. fold its result into the verdict
   // so a half-committed write never reports success.
   int closeRc = closeFs(&file);
   return (written == (int64_t)len && closeRc == 0) ? 0 : -1;
}

// ---- tree measurement ------------------------------------------------------

static uint64_t measureTreeDepth(const char *path, int (*cancelled)(void), int depth)
{
   if (cancelled && cancelled()) return 0;

   VfsStat info;
   if (statPath(path, &info) != 0) return 0;
   if (!info.isDir) return info.size;

   if (depth >= MAX_TREE_DEPTH) return 0;   // too deep: stop before stack overflow

   VfsDir dir;
   if (openDir(path, &dir) != 0) return 0;

   uint64_t sum = 0;
   char name[256];
   char child[MAX_PATH_LEN];
   while (readDir(&dir, name, sizeof name, NULL) == 1) {
      if (cancelled && cancelled()) break;
      if (strEq(name, ".") || strEq(name, "..")) continue;
      if (!joinPath(child, MAX_PATH_LEN, path, name)) continue;   // unjoinable child: skip its bytes
      sum += measureTreeDepth(child, cancelled, depth + 1);
   }
   closeDir(&dir);
   return sum;
}

uint64_t measureTree(const char *path, int (*cancelled)(void))
{
   return measureTreeDepth(path, cancelled, 0);
}

// ---- copy (single source of truth: copyFileProgress / copyTreeRecursively) --

// copies one regular file with optional progress + cancellation. buf is caller
// scratch of bufSize bytes (e.g. 64 KB) - kept caller-provided so this stays
// allocation-free and light on stack, safe to use from prx contexts.
// returns 0 ok, -1 error, 1 cancelled.
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
   // a cancelled or failed copy must not be committed by the close: a backend that
   // publishes on close (an upload finishing) would replace a good file with a partial one.
   out.abandoned = (rc != 0);
   // fold the destination close: a deferred commit error means the copy isn't
   // durable. don't override a cancel (rc == 1) -- that's not a failure.
   int closeRc = closeFs(&out);
   if (closeRc != 0 && rc == 0) rc = -1;
   return rc;
}

// copies a single regular file src -> dst (created/truncated). 0 ok, -1 error.
int copyFile(const char *src, const char *dst, void *buf, int bufSize)
{
   return copyFileProgress(src, dst, buf, bufSize, NULL, NULL);
}

static int copyTreeRecursively(const char *src, const char *dst, void *buf, int bufSize,
                               void (*onBytes)(uint64_t), int (*cancelled)(void), int depth)
{
   if (cancelled && cancelled()) return 1;

   VfsStat info;
   if (statPath(src, &info) != 0) return -1;
   if (!info.isDir) return copyFileProgress(src, dst, buf, bufSize, onBytes, cancelled);

   if (depth >= MAX_TREE_DEPTH) return -1;   // too deep: bail before stack overflow

   if (makeDir(dst) != 0) return -1;

   VfsDir dir;
   if (openDir(src, &dir) != 0) return -1;

   char name[256];
   char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
   int rc = 0;
   int readResult;
   while ((readResult = readDir(&dir, name, sizeof name, NULL)) == 1) {
      if (cancelled && cancelled()) { rc = 1; break; }
      if (strEq(name, ".") || strEq(name, "..")) continue;
      if (!joinPath(childSrc, MAX_PATH_LEN, src, name) ||
          !joinPath(childDst, MAX_PATH_LEN, dst, name)) { rc = -1; break; }
      rc = copyTreeRecursively(childSrc, childDst, buf, bufSize, onBytes, cancelled, depth + 1);
      if (rc != 0) break;
   }
   // a directory-read error (vfs.h: -1) must not pass as a clean copy -- the caller
   // deletes the source on success, so a partial copy would silently lose data.
   if (readResult < 0 && rc == 0) rc = -1;
   closeDir(&dir);
   return rc;
}

// recursively copies src (file or dir) to dst. 0 ok, -1 error.
int copyTree(const char *src, const char *dst, void *buf, int bufSize)
{
   return copyTreeRecursively(src, dst, buf, bufSize, NULL, NULL, 0);
}

int copyTreeProgress(const char *src, const char *dst, void *buf, int bufSize,
                     void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   int rc = copyTreeRecursively(src, dst, buf, bufSize, onBytes, cancelled, 0);

   // one flush at the true batch boundary: makes every byte just written durable
   // and the next free-size read accurate. run even on error/cancel, since a
   // partial copy still left data on the volume. best-effort -- never fails the op.
   syncVfs(dst);
   return rc;
}

// ---- merge -----------------------------------------------------------------

static int mergeTreeRecursively(const char *src, const char *dst, int replaceExisting,
                                void *buf, int bufSize,
                                void (*onBytes)(uint64_t), int (*cancelled)(void), int depth)
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

   if (depth >= MAX_TREE_DEPTH) return -1;   // too deep: bail before stack overflow

   if (makeDir(dst) != 0) return -1;  // no-op when dst already exists

   VfsDir dir;
   if (openDir(src, &dir) != 0) return -1;

   char name[256];
   char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
   int rc = 0;
   int readResult;
   while ((readResult = readDir(&dir, name, sizeof name, NULL)) == 1) {
      if (cancelled && cancelled()) { rc = 1; break; }
      if (strEq(name, ".") || strEq(name, "..")) continue;
      if (!joinPath(childSrc, MAX_PATH_LEN, src, name) ||
          !joinPath(childDst, MAX_PATH_LEN, dst, name)) { rc = -1; break; }
      rc = mergeTreeRecursively(childSrc, childDst, replaceExisting, buf, bufSize, onBytes, cancelled, depth + 1);
      if (rc != 0) break;
   }
   // a directory-read error (vfs.h: -1) must not pass as a clean merge.
   if (readResult < 0 && rc == 0) rc = -1;
   closeDir(&dir);
   return rc;
}

int mergeTreeProgress(const char *src, const char *dst, int replaceExisting,
                      void *buf, int bufSize,
                      void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   int rc = mergeTreeRecursively(src, dst, replaceExisting, buf, bufSize, onBytes, cancelled, 0);

   syncVfs(dst);
   return rc;
}

// ---- conflict counting -----------------------------------------------------

static int countTreeConflictsDepth(const char *src, const char *dst, int cap, int depth)
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

   if (depth >= MAX_TREE_DEPTH) return 0;   // too deep: stop counting before stack overflow

   VfsDir dir;
   if (openDir(src, &dir) != 0) return 0;

   char name[256];
   char childSrc[MAX_PATH_LEN], childDst[MAX_PATH_LEN];
   int count = 0;
   while (count < cap && readDir(&dir, name, sizeof name, NULL) == 1) {
      if (strEq(name, ".") || strEq(name, "..")) continue;
      if (!joinPath(childSrc, MAX_PATH_LEN, src, name) ||
          !joinPath(childDst, MAX_PATH_LEN, dst, name)) continue;
      count += countTreeConflictsDepth(childSrc, childDst, cap - count, depth + 1);
   }
   closeDir(&dir);
   return count;
}

int countTreeConflicts(const char *src, const char *dst, int cap)
{
   return countTreeConflictsDepth(src, dst, cap, 0);
}

// ---- delete ----------------------------------------------------------------
// Two variants are kept deliberately: deleteTreeProgress reports bytes per file
// through an onBytes callback; deleteTree accumulates the total into a uint64_t*
// out-param. C callbacks carry no context pointer, so one cannot be expressed as
// a thin wrapper over the other without changing the public callback signature -
// the duplication is the minimal cost of staying source-compatible.

static int deleteTreeRecursively(const char *path, void (*onBytes)(uint64_t), int (*cancelled)(void), int depth)
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

   if (depth >= MAX_TREE_DEPTH) return -1;   // too deep: bail before stack overflow

   VfsDir dir;
   if (openDir(path, &dir) != 0) return -1;

   char name[256];
   char child[MAX_PATH_LEN];
   int rc = 0;
   int readResult;
   while ((readResult = readDir(&dir, name, sizeof name, NULL)) == 1) {
      if (cancelled && cancelled()) { rc = 1; break; }
      if (strEq(name, ".") || strEq(name, "..")) continue;
      if (!joinPath(child, MAX_PATH_LEN, path, name)) { rc = -1; break; }
      rc = deleteTreeRecursively(child, onBytes, cancelled, depth + 1);
      if (rc != 0) break;
   }
   // a directory-read error (vfs.h: -1) must not pass as a clean delete.
   if (readResult < 0 && rc == 0) rc = -1;
   closeDir(&dir);
   if (rc != 0) return rc;

   return removeDirPath(path) == 0 ? 0 : -1;
}

int deleteTreeProgress(const char *path, void (*onBytes)(uint64_t), int (*cancelled)(void))
{
   if (isDeviceRoot(path)) return -1;   // never delete a whole device root
   int rc = deleteTreeRecursively(path, onBytes, cancelled, 0);

   // flush the directory-entry removals and freed blocks to disk once, so the
   // volume can't be left mid-update and the freed space shows up immediately.
   syncVfs(path);
   return rc;
}

static int deleteTreeDepth(const char *path, uint64_t *bytesFreed, int depth)
{
   VfsStat info;
   if (statPath(path, &info) != 0) return removeFilePath(path);

   if (!info.isDir) {
      int rc = deleteFile(path);
      if (rc == 0 && bytesFreed) *bytesFreed += info.size;   // count only what was actually removed
      return rc;
   }

   if (depth >= MAX_TREE_DEPTH) return -1;   // too deep: bail before stack overflow

   VfsDir dir;
   if (openDir(path, &dir) != 0) return -1;

   char name[256], child[MAX_PATH_LEN];
   int readResult;
   while ((readResult = readDir(&dir, name, sizeof name, NULL)) == 1) {
      if (strEq(name, ".") || strEq(name, "..")) continue;
      if (!joinPath(child, MAX_PATH_LEN, path, name)) { closeDir(&dir); return -1; }
      if (deleteTreeDepth(child, bytesFreed, depth + 1) < 0) { closeDir(&dir); return -1; }
   }
   closeDir(&dir);
   if (readResult < 0) return -1;   // directory read error: a partial walk must not claim success

   return removeDirPath(path) == 0 ? 0 : -1;
}

// recursively deletes path (file or dir). adds the size of every removed
// regular file into *bytesFreed (pass NULL to ignore). idempotent when absent.
int deleteTree(const char *path, uint64_t *bytesFreed)
{
   if (isDeviceRoot(path)) return -1;   // never delete a whole device root
   return deleteTreeDepth(path, bytesFreed, 0);
}

// ---- move ------------------------------------------------------------------

int moveTree(const char *src, const char *dst, void *buf, int bufSize)
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
   char srcRoot[34], dstRoot[34];   // '/' + 31-char mount segment + NUL needs 33
   deviceRootOf(src, srcRoot, sizeof srcRoot);
   deviceRootOf(dst, dstRoot, sizeof dstRoot);
   syncVfs(dst);
   if (!strEq(srcRoot, dstRoot)) syncVfs(src);
   return rc;
}
