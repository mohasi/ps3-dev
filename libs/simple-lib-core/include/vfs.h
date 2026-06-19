#pragma once
//
// vfs.h - one filesystem abstraction for the whole codebase.
//
// every consumer (apps, plugins, libs) reaches the filesystem through this
// header and the file.h helpers built on it. callers pass normal '/'-rooted
// paths and never learn whether a path lives on FAT32/HDD (cellFs), NTFS or
// exFAT - the VFS routes each call to the right backend.
//
// namespace model (see VFS-DESIGN.md for the rationale):
//   /                 synthetic root
//   /dev_hdd0/...     cellFs   (identity translation)
//   /dev_usb000/...   cellFs   (FAT32, kernel-mounted)
//   /ntfs0/...        NTFS     -> "ntfs0:/..."   (translated inside the VFS)
//   /exfat0/...       exFAT    -> "exFAT0:/..."  (translated inside the VFS)
//
// a consumer-visible path is always '/'-rooted and '/'-separated; the colon
// scheme libntfs/FatFs use is private to the VFS. virtual NTFS/exFAT volumes
// appear as directories at root, beside the real cellFs devices.

#include <stdint.h>

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 512
#endif

// normalized metadata. the UI binds size/mtime/isDir; mode is the real POSIX
// permission bits for backends that have them (cellFs, NTFS) and synthesized
// where the filesystem has none (exFAT). consumed by the FTP listing.
typedef struct {
   uint64_t size;          // bytes (regular files; dir size is filled by the sizer)
   uint64_t mtime;         // seconds, unix-ish (exFAT is coarse: DOS 2s, no TZ)
   int      isDir;
   uint32_t mode;          // st_mode-style bits; use & 0777 for unix permissions
} VfsStat;

// opaque handles, deliberately small: the folder-sizer keeps a stack of 64 open
// dir handles on an 8 KB thread, so these stay a few words. backend state larger
// than an int (e.g. a FatFs FDIR) lives in a pool inside vfs.c, keyed by descriptor.
typedef struct {
   const void *backend;        // FsOps vtable (private)
   int         descriptor;     // cellFs file descriptor, or pool slot
   void       *nativeHandle;   // backend pointer (e.g. DIR_ITER*) when an int isn't enough
} VfsDir;

typedef struct {
   const void *backend;
   int         descriptor;     // cellFs/NTFS file descriptor, or exFAT pool slot
} VfsFile;

// open flags for openFs (backend-neutral; mapped per backend)
#define VFS_O_RDONLY 0x0001
#define VFS_O_WRONLY 0x0002
#define VFS_O_RDWR   0x0004
#define VFS_O_CREAT  0x0008
#define VFS_O_TRUNC  0x0010
#define VFS_O_APPEND 0x0020

// whence for seekFs
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

typedef enum { VFS_SCHEME_CELLFS, VFS_SCHEME_NTFS, VFS_SCHEME_EXFAT, VFS_SCHEME_EXT } VfsScheme;

// a backend's operation table. cellFs is built into vfs.c; NTFS/exFAT supply their
// own and register it (below). keeping backends out of the core means the heavy
// libntfs/FatFs code links only into the app that wants them, never into the lean
// PRX plugins that include file.h just for the helpers.
typedef struct VfsOps {
   int     (*stat)    (const char *native, VfsStat *outStat);
   int     (*rename)  (const char *from, const char *to);
   int     (*mkdir)   (const char *native);
   int     (*rmfile)  (const char *native);
   int     (*rmdir)   (const char *native);
   int     (*getfree) (const char *native, uint64_t *freeBytes, uint64_t *totalBytes);
   int     (*opendir) (const char *native, VfsDir *dir);
   int     (*readdir) (VfsDir *dir, char *nameOut, int nameCapacity, int *isDirOut);
   void    (*closedir)(VfsDir *dir);
   int     (*open)    (const char *native, int flags, VfsFile *file);
   int64_t (*read)    (VfsFile *file, void *buffer, uint64_t length);
   int64_t (*write)   (VfsFile *file, const void *buffer, uint64_t length);
   int64_t (*seek)    (VfsFile *file, int64_t offset, int whence);
   int     (*fsync)   (VfsFile *file);
   void    (*close)   (VfsFile *file);
} VfsOps;

// A format backend's answer when the VFS offers it a present storage device (see below).
typedef enum {
   VFS_PROBE_MOUNTED,    // it's my format and I mounted it (published via vfsAddMount)
   VFS_PROBE_NOT_READY,  // device present but not readable yet (no backend can read it) - retry later
   VFS_PROBE_NOT_MINE    // read fine, not my format - offer it to the next backend
} VfsProbeResult;

// Backend registration. The VFS owns USB hotplug detection (device presence is format-agnostic);
// a backend's start routine (which only the app links) registers three hooks and never polls the
// ports itself. For each newly-present device the VFS calls probe(port) on backends in turn until
// one returns VFS_PROBE_MOUNTED; the winner publishes the volume with vfsAddMount. When that
// device is removed the VFS calls the owner's release(port) (which calls vfsRemoveMount). shutdown
// runs at exit. A device no backend claims is left to cellFs (e.g. FAT32) - the VFS still refreshes
// the listing on every presence change, so such devices appear without any backend's involvement.
void registerVfsBackend(VfsProbeResult (*probe)(int port), void (*release)(int port), void (*shutdown)(void));
int  vfsAddMount(const char *segment, const char *native, const char *label, VfsScheme scheme, const VfsOps *ops);
void vfsRemoveMount(const char *segment);

// a virtual mount surfaced into the root listing.
typedef struct {
   char      segment[32];      // root segment; the path is "/" + segment (e.g. "ntfs0")
   char      label[64];        // friendly volume name for display (falls back to segment)
   VfsScheme scheme;
   uint64_t  freeBytes;        // best-effort, 0 until queried
   uint64_t  totalBytes;       // best-effort, may be 0
} VfsMount;

// lifecycle
void initVfs(void);             // bring up backends + initial mount sweep; call once after mountDevBlind
int  pollMounts(void);          // re-scan USB hotplug (debounced); call ~1 Hz; 1 if the mount set changed
void setMountsChangedCallback(void (*callback)(void));   // fired by pollMounts when the mount set changes
void shutdownVfs(void);         // unmount everything; call on exit

// mount / root enumeration
int       listMounts(VfsMount *outMounts, int capacity);   // copies the virtual mounts; returns the count
VfsScheme getScheme(const char *path);                     // which backend services path

// metadata / namespace
int  statPath(const char *path, VfsStat *outStat);          // 0 / -1
int  renamePath(const char *oldPath, const char *newPath);  // same-volume only (-1 cross-volume; use moveTree)
int  makeDirPath(const char *path);                         // 0 if created or already present
int  removeFilePath(const char *path);                      // 0 if removed or already absent
int  removeDirPath(const char *path);                       // 0 on success (empty dir)
void syncVfs(const char *path);                             // durability flush for path's volume
int  getFreeSpace(const char *path, uint64_t *freeBytes, uint64_t *totalBytes);   // total 0 if unknown

// directory iteration (long-lived handle model; matches loadDir / walkPath)
int  openDir(const char *path, VfsDir *dir);                                // 0 / -1
int  readDir(VfsDir *dir, char *nameOut, int nameCapacity, int *isDirOut);  // 1 entry, 0 end, -1 error
void closeDir(VfsDir *dir);

// file i/o (file.h's readFile/writeFile/copyFile build on these)
int     openFs(const char *path, int flags, VfsFile *file);             // 0 / -1
int64_t readFs(VfsFile *file, void *buffer, uint64_t length);           // bytes, or -1
int64_t writeFs(VfsFile *file, const void *buffer, uint64_t length);    // bytes, or -1
int64_t seekFs(VfsFile *file, int64_t offset, int whence);              // new position, or -1
int     fsyncFs(VfsFile *file);                                         // 0 / -1
void    closeFs(VfsFile *file);
