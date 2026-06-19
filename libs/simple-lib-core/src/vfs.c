//
// vfs.c - filesystem abstraction: path-scheme router + per-backend vtables.
//
// backends (each registers a probe/release pair; the VFS owns hotplug - see registerVfsBackend):
//   cellFs - HDD + kernel-mounted FAT32 USB. always present. identity paths.
//   exFAT  - hand-written exfat.c. registered and live (read/write/create/delete/rename).
//   NTFS   - libntfs_ext (ps3ntfs_*). not yet added; drops in as another probe/release backend.
//
// resolvePath peels the first path segment, matches it against the virtual-mount
// registry, and rewrites the path to the backend's native form ("/ntfs0/x" ->
// "ntfs0:/x"). unmatched paths are cellFs and pass through unchanged, so
// HDD/FAT32 behaviour is identical to before the VFS existed.
//
// see VFS-DESIGN.md for the design and the reference-app source map.

#include "vfs.h"

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sys/fs_external.h>
#include <sys/sys_time.h>       // sys_time_get_system_time (pollMounts self-throttle)
#include "string-utilities.h"   // strCopy, strEq
#include "syscall.h"            // syncDevice
#include "usb-storage.h"        // isUsbDevicePresent + port count (format-agnostic hotplug)
#include "exfat.h"              // initExfat (brought up as part of the VFS)

#define VFS_POLL_INTERVAL_US 1000000   // scan hotplug at most once a second, however often callers poll

#define VFS_MAX_MOUNTS 24       // ntfs0..7 + exfat0..7 + ext0..7

// VfsOps is the public vtable from vfs.h. cellFs ops are built in below;
// NTFS/exFAT register theirs at runtime so their code never links into core.

// virtual mounts only (NTFS/exFAT/ext). cellFs devices are the default route and
// are real children of "/", so they are never registered here.
typedef struct {
   char                 segment[32];   // root segment, e.g. "ntfs0"
   char                 native[40];    // native prefix, e.g. "ntfs0:"
   char                 label[64];
   VfsScheme            scheme;
   const VfsOps *backend;
   int                  present;
} MountEntry;

static MountEntry mounts[VFS_MAX_MOUNTS];
static int        mountCount;
static int        initialized;

// Registered format backends (probe/release/shutdown hooks); set by the app, never by core, so
// libntfs/FatFs link only where they're actually used. The VFS owns USB hotplug detection and
// offers present devices to these backends - they never poll the ports themselves.
#define VFS_MAX_BACKENDS 4
static struct {
   VfsProbeResult (*probe)(int port);
   void           (*release)(int port);
   void           (*shutdown)(void);
} backends[VFS_MAX_BACKENDS];
static int backendCount;

// Per-USB-port hotplug state, owned by the VFS (format-agnostic). present: a device is on the
// port. resolved: we've settled what it is (a backend mounted it, or all declined / cellFs).
// owner: index of the backend that mounted it, or -1.
static uint8_t portPresent[USB_STORAGE_MAX_PORTS];
static uint8_t portResolved[USB_STORAGE_MAX_PORTS];
static int8_t  portOwner[USB_STORAGE_MAX_PORTS];

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

static int isDotEntry(const char *name)
{
   return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// section: cellFs backend - thin wrappers over the calls the codebase used
// directly, so HDD/FAT32 behaviour is unchanged.

static int statCellFs(const char *native, VfsStat *outStat)
{
   CellFsStat info;
   if (cellFsStat(native, &info) != CELL_FS_SUCCEEDED) return -1;
   outStat->size  = info.st_size;
   outStat->mtime = (uint64_t)info.st_mtime;
   outStat->isDir = (info.st_mode & CELL_FS_S_IFDIR) != 0;
   outStat->mode  = (uint32_t)info.st_mode;
   return 0;
}

static int renameCellFs(const char *from, const char *to)
{
   return cellFsRename(from, to) == CELL_FS_SUCCEEDED ? 0 : -1;
}

static int makeDirCellFs(const char *native)
{
   int result = cellFsMkdir(native, CELL_FS_S_IFDIR | 0777);
   return (result == CELL_FS_SUCCEEDED || result == (int)CELL_FS_EEXIST) ? 0 : -1;
}

static int removeFileCellFs(const char *native)
{
   int result = cellFsUnlink(native);
   return (result == CELL_FS_SUCCEEDED || result == (int)CELL_FS_ENOENT) ? 0 : -1;
}

static int removeDirCellFs(const char *native)
{
   return cellFsRmdir(native) == CELL_FS_SUCCEEDED ? 0 : -1;
}

static int getFreeCellFs(const char *native, uint64_t *freeBytes, uint64_t *totalBytes)
{
   uint32_t blockSize = 0;
   uint64_t freeBlocks = 0;
   if (cellFsGetFreeSize(native, &blockSize, &freeBlocks) != CELL_FS_SUCCEEDED) return -1;
   if (freeBytes)  *freeBytes  = (uint64_t)blockSize * freeBlocks;
   if (totalBytes) *totalBytes = 0;   // cellFs has no cheap total; 0 means "unknown"
   return 0;
}

static int openDirCellFs(const char *native, VfsDir *dir)
{
   int descriptor;
   if (cellFsOpendir(native, &descriptor) != CELL_FS_SUCCEEDED) return -1;
   dir->descriptor   = descriptor;
   dir->nativeHandle = 0;
   return 0;
}

static int readDirCellFs(VfsDir *dir, char *nameOut, int nameCapacity, int *isDirOut)
{
   CellFsDirent entry;
   uint64_t bytesRead = 0;
   while (cellFsReaddir(dir->descriptor, &entry, &bytesRead) == CELL_FS_SUCCEEDED && bytesRead > 0) {
      if (isDotEntry(entry.d_name)) continue;
      strCopy(nameOut, nameCapacity, entry.d_name);
      if (isDirOut) *isDirOut = (entry.d_type == CELL_FS_TYPE_DIRECTORY);
      return 1;
   }
   return 0;
}

static void closeDirCellFs(VfsDir *dir)
{
   cellFsClosedir(dir->descriptor);
   dir->descriptor = -1;
}

static int openCellFs(const char *native, int flags, VfsFile *file)
{
   int cellFlags = 0;
   if (flags & VFS_O_WRONLY) cellFlags |= CELL_FS_O_WRONLY;
   if (flags & VFS_O_RDWR)   cellFlags |= CELL_FS_O_RDWR;
   if (!(flags & (VFS_O_WRONLY | VFS_O_RDWR))) cellFlags |= CELL_FS_O_RDONLY;
   if (flags & VFS_O_CREAT)  cellFlags |= CELL_FS_O_CREAT;
   if (flags & VFS_O_TRUNC)  cellFlags |= CELL_FS_O_TRUNC;
   if (flags & VFS_O_APPEND) cellFlags |= CELL_FS_O_APPEND;
   int descriptor;
   if (cellFsOpen(native, cellFlags, &descriptor, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
   file->descriptor = descriptor;
   return 0;
}

static int64_t readCellFs(VfsFile *file, void *buffer, uint64_t length)
{
   uint64_t bytesRead = 0;
   if (cellFsRead(file->descriptor, buffer, length, &bytesRead) != CELL_FS_SUCCEEDED) return -1;
   return (int64_t)bytesRead;
}

static int64_t writeCellFs(VfsFile *file, const void *buffer, uint64_t length)
{
   uint64_t bytesWritten = 0;
   if (cellFsWrite(file->descriptor, buffer, length, &bytesWritten) != CELL_FS_SUCCEEDED) return -1;
   return (int64_t)bytesWritten;
}

static int64_t seekCellFs(VfsFile *file, int64_t offset, int whence)
{
   int cellWhence = (whence == VFS_SEEK_CUR) ? CELL_FS_SEEK_CUR
         : (whence == VFS_SEEK_END) ? CELL_FS_SEEK_END : CELL_FS_SEEK_SET;
   uint64_t position = 0;
   if (cellFsLseek(file->descriptor, offset, cellWhence, &position) != CELL_FS_SUCCEEDED) return -1;
   return (int64_t)position;
}

static int fsyncCellFs(VfsFile *file)
{
   (void)file;   // cellFs durability is path-level (syncDevice); see syncVfs
   return 0;
}

static void closeCellFs(VfsFile *file)
{
   cellFsClose(file->descriptor);
   file->descriptor = -1;
}

static const VfsOps CELLFS_OPS = {
   statCellFs, renameCellFs, makeDirCellFs, removeFileCellFs, removeDirCellFs, getFreeCellFs,
   openDirCellFs, readDirCellFs, closeDirCellFs,
   openCellFs, readCellFs, writeCellFs, seekCellFs, fsyncCellFs, closeCellFs,
};

// section: synthetic "/" listing - cellFs root devices first, then the virtual
// mounts, so loadDir() needs no special case. descriptor holds the cellFs "/"
// handle while phase 1 runs; once drained it is -1 and nativeHandle carries the
// registry cursor for phase 2.

static int openDirRoot(const char *native, VfsDir *dir)
{
   (void)native;
   int descriptor;
   if (cellFsOpendir("/", &descriptor) != CELL_FS_SUCCEEDED) descriptor = -1;   // phase 2 still runs
   dir->descriptor   = descriptor;
   dir->nativeHandle = 0;
   return 0;
}

static int readDirRoot(VfsDir *dir, char *nameOut, int nameCapacity, int *isDirOut)
{
   // phase 1: real cellFs root entries, filtered to those userland can enter
   while (dir->descriptor >= 0) {
      CellFsDirent entry;
      uint64_t bytesRead = 0;
      if (cellFsReaddir(dir->descriptor, &entry, &bytesRead) != CELL_FS_SUCCEEDED || bytesRead == 0) {
         cellFsClosedir(dir->descriptor);
         dir->descriptor = -1;
         break;
      }
      if (isDotEntry(entry.d_name)) continue;

      char probePath[64];
      probePath[0] = '/';
      strCopy(probePath + 1, (int)sizeof probePath - 1, entry.d_name);

      int probe;
      if (cellFsOpendir(probePath, &probe) != CELL_FS_SUCCEEDED) continue;   // unenterable (devkit/system)
      cellFsClosedir(probe);

      strCopy(nameOut, nameCapacity, entry.d_name);
      if (isDirOut) *isDirOut = 1;
      return 1;
   }

   // phase 2: virtual NTFS/exFAT mounts from the registry
   int cursor = (int)(intptr_t)dir->nativeHandle;
   while (cursor < mountCount) {
      MountEntry *mount = &mounts[cursor++];
      if (!mount->present) continue;
      strCopy(nameOut, nameCapacity, mount->segment);
      if (isDirOut) *isDirOut = 1;
      dir->nativeHandle = (void *)(intptr_t)cursor;
      return 1;
   }
   dir->nativeHandle = (void *)(intptr_t)cursor;
   return 0;
}

static void closeDirRoot(VfsDir *dir)
{
   if (dir->descriptor >= 0) { cellFsClosedir(dir->descriptor); dir->descriptor = -1; }
}

static const VfsOps ROOT_OPS = {
   statCellFs, renameCellFs, makeDirCellFs, removeFileCellFs, removeDirCellFs, getFreeCellFs,
   openDirRoot, readDirRoot, closeDirRoot,
   openCellFs, readCellFs, writeCellFs, seekCellFs, fsyncCellFs, closeCellFs,
};

// section: path resolution

// resolves a consumer path to its backend and native form. cellFs paths are used
// verbatim (no copy); virtual-mount paths are rewritten into buffer.
static const VfsOps *resolvePath(const char *path, char *buffer, int capacity, const char **native)
{
   char segment[32];
   const char *rest = splitFirstSegment(path, segment, sizeof segment);
   MountEntry *mount = findMount(segment);
   if (!mount) { *native = path; return &CELLFS_OPS; }

   // native = prefix + rest with exactly one '/' joining them ("/ntfs0/x" -> "ntfs0:/x")
   int length = 0;
   const char *prefix = mount->native;
   while (prefix[length] && length < capacity - 1) { buffer[length] = prefix[length]; length++; }
   if (length < capacity - 1) buffer[length++] = '/';
   const char *tail = (rest[0] == '/') ? rest + 1 : rest;
   while (*tail && length < capacity - 1) buffer[length++] = *tail++;
   buffer[length] = '\0';
   *native = buffer;
   return mount->backend;
}

// section: lifecycle

// cellFs is the built-in default route; the removable-media backends are brought
// up here so any VFS consumer sees them without a separate call. exFAT is small
// and prx-safe (no libc - just scCall + memCopy), so it can ride into the plugins
// that link the VFS. The heavier NTFS backend will start the same way once added.
void initVfs(void)
{
   if (initialized) return;
   initialized  = 1;
   mountCount   = 0;
   backendCount = 0;
   for (int p = 0; p < USB_STORAGE_MAX_PORTS; p++) { portPresent[p] = 0; portResolved[p] = 0; portOwner[p] = -1; }
   initExfat();    // registers the exFAT backend (NTFS will register the same way)
   pollMounts();   // initial scan so already-inserted volumes appear immediately
}

// invoked by pollMounts when the mount set changes, on whatever thread polled.
static void (*mountsChangedCallback)(void);

void setMountsChangedCallback(void (*callback)(void))
{
   mountsChangedCallback = callback;
}

// drives each registered backend's hotplug scan, self-throttled to VFS_POLL_INTERVAL_US
// so any caller (the app loop, an ftp listener, ...) can call it as often as it likes
// without putting the sys_storage probe on a hot path. when the mount set changes it
// fires the mounts-changed callback (so a consumer's view refreshes without the caller
// having to inspect the return value), and also returns 1.
int pollMounts(void)
{
   static system_time_t lastScan;   // 0 until the first scan
   system_time_t now = sys_time_get_system_time();
   if (lastScan && now - lastScan < VFS_POLL_INTERVAL_US) return 0;
   lastScan = now;

   int changed = 0;
   for (int port = 0; port < USB_STORAGE_MAX_PORTS; port++) {
      int present = isUsbDevicePresent(port);   // device-level, format-agnostic (non-DMA, no LED)

      if (present && !portPresent[port]) {                  // device inserted
         portPresent[port]  = 1;
         portResolved[port] = 0;
         portOwner[port]    = -1;
         changed = 1;                                       // refresh now - also surfaces cellFs/FAT32
      } else if (!present && portPresent[port]) {           // device removed
         portPresent[port] = 0;
         if (portOwner[port] >= 0) backends[portOwner[port]].release(port);
         portOwner[port]    = -1;
         portResolved[port] = 0;
         changed = 1;
      }

      // Offer an unresolved present device to each backend until one claims it. NOT_READY means
      // the device can't be read yet (no backend could) - leave it unresolved to retry next poll.
      // If every backend declines, it's a format we don't mount (cellFs handles it, e.g. FAT32).
      if (present && !portResolved[port]) {
         int notReady = 0;
         for (int i = 0; i < backendCount; i++) {
            VfsProbeResult r = backends[i].probe(port);
            if (r == VFS_PROBE_MOUNTED)   { portOwner[port] = (int8_t)i; portResolved[port] = 1; changed = 1; break; }
            if (r == VFS_PROBE_NOT_READY) { notReady = 1; break; }
            // VFS_PROBE_NOT_MINE: try the next backend
         }
         if (!portResolved[port] && !notReady) portResolved[port] = 1;   // none claimed it
      }
   }

   if (changed && mountsChangedCallback) mountsChangedCallback();
   return changed;
}

void shutdownVfs(void)
{
   for (int i = 0; i < backendCount; i++)
      if (backends[i].shutdown) backends[i].shutdown();
   mountCount   = 0;
   backendCount = 0;
   initialized  = 0;
}

void registerVfsBackend(VfsProbeResult (*probe)(int port), void (*release)(int port), void (*shutdown)(void))
{
   if (backendCount >= VFS_MAX_BACKENDS) return;
   backends[backendCount].probe    = probe;
   backends[backendCount].release  = release;
   backends[backendCount].shutdown = shutdown;
   backendCount++;
}

// publishes a mounted volume. reuses a withdrawn slot when one is free so repeated
// hotplug cycles can't exhaust the table. returns 0, or -1 if the table is full.
int vfsAddMount(const char *segment, const char *native, const char *label, VfsScheme scheme, const VfsOps *ops)
{
   MountEntry *mount = NULL;
   for (int i = 0; i < mountCount; i++)
      if (!mounts[i].present) { mount = &mounts[i]; break; }
   if (!mount) {
      if (mountCount >= VFS_MAX_MOUNTS) return -1;
      mount = &mounts[mountCount++];
   }
   strCopy(mount->segment, sizeof mount->segment, segment);
   strCopy(mount->native,  sizeof mount->native,  native);
   strCopy(mount->label,   sizeof mount->label,   label && label[0] ? label : segment);
   mount->scheme  = scheme;
   mount->backend = ops;
   mount->present = 1;
   return 0;
}

void vfsRemoveMount(const char *segment)
{
   MountEntry *mount = findMount(segment);
   if (mount) mount->present = 0;
}

// section: public api

int listMounts(VfsMount *outMounts, int capacity)
{
   int count = 0;
   for (int i = 0; i < mountCount && count < capacity; i++) {
      if (!mounts[i].present) continue;
      strCopy(outMounts[count].segment, sizeof outMounts[count].segment, mounts[i].segment);
      strCopy(outMounts[count].label,   sizeof outMounts[count].label,   mounts[i].label);
      outMounts[count].scheme     = mounts[i].scheme;
      outMounts[count].freeBytes  = 0;
      outMounts[count].totalBytes = 0;
      count++;
   }
   return count;
}

VfsScheme getScheme(const char *path)
{
   char segment[32];
   splitFirstSegment(path, segment, sizeof segment);
   MountEntry *mount = findMount(segment);
   return mount ? mount->scheme : VFS_SCHEME_CELLFS;
}

int statPath(const char *path, VfsStat *outStat)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   return resolvePath(path, buffer, sizeof buffer, &native)->stat(native, outStat);
}

int renamePath(const char *oldPath, const char *newPath)
{
   char fromBuffer[MAX_PATH_LEN], toBuffer[MAX_PATH_LEN];
   const char *from, *to;
   const VfsOps *fromBackend = resolvePath(oldPath, fromBuffer, sizeof fromBuffer, &from);
   const VfsOps *toBackend   = resolvePath(newPath, toBuffer,   sizeof toBuffer,   &to);
   if (fromBackend != toBackend) return -1;   // cross-volume: caller falls back to moveTree
   return fromBackend->rename(from, to);
}

int makeDirPath(const char *path)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   return resolvePath(path, buffer, sizeof buffer, &native)->mkdir(native);
}

int removeFilePath(const char *path)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   return resolvePath(path, buffer, sizeof buffer, &native)->rmfile(native);
}

int removeDirPath(const char *path)
{
   char buffer[MAX_PATH_LEN];
   const char *native;
   return resolvePath(path, buffer, sizeof buffer, &native)->rmdir(native);
}

void syncVfs(const char *path)
{
   if (getScheme(path) != VFS_SCHEME_CELLFS) return;   // userland backends flush per-file (fsync)
   char root[16];
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
   return resolvePath(path, buffer, sizeof buffer, &native)->getfree(native, freeBytes, totalBytes);
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
   dir->backend = backend;
   return backend->opendir(native, dir);
}

int readDir(VfsDir *dir, char *nameOut, int nameCapacity, int *isDirOut)
{
   return ((const VfsOps *)dir->backend)->readdir(dir, nameOut, nameCapacity, isDirOut);
}

void closeDir(VfsDir *dir)
{
   ((const VfsOps *)dir->backend)->closedir(dir);
}

int openFs(const char *path, int flags, VfsFile *file)
{
   file->descriptor = -1;
   char buffer[MAX_PATH_LEN];
   const char *native;
   const VfsOps *backend = resolvePath(path, buffer, sizeof buffer, &native);
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

void closeFs(VfsFile *file)
{
   ((const VfsOps *)file->backend)->close(file);
}
