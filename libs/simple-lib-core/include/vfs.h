#pragma once
//
// vfs.h - one filesystem abstraction for the whole codebase.
//
// every consumer (apps, plugins, libs) reaches the filesystem through this one
// header. callers pass normal '/'-rooted paths and never learn whether a path
// lives on FAT32/HDD (cellFs), NTFS or exFAT - the VFS routes each call to the
// right backend. The low-level primitives (open/read/dir/stat) and the
// cross-backend operations built on them (readFile/copyTree/moveTree/...) are
// all declared here; pure path-string and size-format helpers come in via
// path.h / format.h below.
//
// namespace model (see VFS-DESIGN.md for the rationale):
//   /                 synthetic root
//   /dev_hdd0/...     cellFs   (identity translation)
//   /dev_usb000/...   cellFs   (FAT32, kernel-mounted)
//   /ntfs0/...        NTFS     -> "ntfs0:/..."   (translated inside the VFS)
//   /exfat0/...       exFAT    -> "exfat0:/..."  (translated inside the VFS)
//
// a consumer-visible path is always '/'-rooted and '/'-separated; the colon
// scheme libntfs/FatFs use is private to the VFS. virtual NTFS/exFAT volumes
// appear as directories at root, beside the real cellFs devices.

#include <stdint.h>
#include "path.h"      // MAX_PATH_LEN + pure path-string helpers (joinPath, getBaseName, ...)
#include "format.h"    // formatSize / formatSizeApprox

// deepest directory nesting the recursive tree walkers descend before aborting
// with -1 instead of recursing further. each native frame holds name[256] plus
// one or two MAX_PATH_LEN buffers (~1.3 KB), so unbounded recursion blows a small
// PRX/thread stack mid-mutation (no rollback). 32 levels stays comfortably inside
// the 64 KB process stack these ops run on while covering any realistic tree.
#define MAX_TREE_DEPTH 32

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

// directory-entry kind reported by readDir. SYMLINK is classified WITHOUT
// following the link (lstat-style); backends that have no symlinks (e.g. exFAT)
// simply never report it. OTHER covers devices/fifos/sockets and the like.
typedef enum {
   VFS_ENTRY_FILE,
   VFS_ENTRY_DIR,
   VFS_ENTRY_SYMLINK,
   VFS_ENTRY_OTHER
} VfsEntryType;

// a backend's operation table. cellFs lives in cellfs.c (the default route);
// NTFS/exFAT supply their own and register it (below). keeping backends in their
// own files means the heavy libntfs/FatFs code links only into the binaries that
// actually use them, and the router (vfs.c) names no concrete backend.
typedef struct VfsOps {
   int     (*stat)    (const char *native, VfsStat *outStat);
   int     (*rename)  (const char *from, const char *to);
   int     (*mkdir)   (const char *native);
   int     (*rmfile)  (const char *native);
   int     (*rmdir)   (const char *native);
   int     (*getfree) (const char *native, uint64_t *freeBytes, uint64_t *totalBytes);
   int     (*opendir) (const char *native, VfsDir *dir);
   int     (*readdir) (VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut);
   void    (*closedir)(VfsDir *dir);
   int     (*open)    (const char *native, int flags, VfsFile *file);
   int64_t (*read)    (VfsFile *file, void *buffer, uint64_t length);
   int64_t (*write)   (VfsFile *file, const void *buffer, uint64_t length);
   int64_t (*seek)    (VfsFile *file, int64_t offset, int whence);
   int     (*fsync)   (VfsFile *file);
   int     (*close)   (VfsFile *file);   // 0 / -1; surfaces a commit error deferred to close
} VfsOps;

// A format backend's answer when the VFS offers it a present storage device (see below).
typedef enum {
   VFS_PROBE_MOUNTED,    // it's my format and I mounted it (published via addVfsMount)
   VFS_PROBE_NOT_READY,  // device present but not readable yet (no backend can read it) - retry later
   VFS_PROBE_NOT_MINE    // read fine, not my format - offer it to the next backend
} VfsProbeResult;

// Backend registration. The VFS owns USB hotplug detection (device presence is format-agnostic);
// a backend's start routine (which only the app links) registers three hooks and never polls the
// ports itself. For each newly-present device the VFS calls probe(port) on backends in turn until
// one returns VFS_PROBE_MOUNTED; the winner publishes the volume with addVfsMount. When that
// device is removed the VFS calls the owner's release(port) (which calls removeVfsMount). shutdown
// runs at exit. A device no backend claims is left to cellFs (e.g. FAT32) - the VFS still refreshes
// the listing on every presence change, so such devices appear without any backend's involvement.
void registerVfsBackend(VfsProbeResult (*probe)(int port), void (*release)(int port), void (*shutdown)(void));
int  addVfsMount(const char *segment, const char *native, const char *label, VfsScheme scheme, const VfsOps *ops);
void removeVfsMount(const char *segment);

// a virtual mount surfaced into the root listing.
typedef struct {
   char      segment[32];      // root segment; the path is "/" + segment (e.g. "ntfs0")
   char      label[64];        // friendly volume name for display (falls back to segment)
   VfsScheme scheme;
   uint64_t  freeBytes;        // best-effort, 0 until queried
   uint64_t  totalBytes;       // best-effort, may be 0
} VfsMount;

// lifecycle
void initVfs(void);             // bring up backends + start the 8 KB hotplug poll thread; call once
void setMountsChangedCallback(void (*callback)(void));   // fired when the mount set changes (on the poll thread)
void shutdownVfs(void);         // stop the poll thread + unmount everything; call on exit

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
int  readDir(VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut);  // 1 entry, 0 end, -1 error (typeOut may be NULL)
void closeDir(VfsDir *dir);

// file i/o primitives (the cross-backend helpers below build on these)
int     openFs(const char *path, int flags, VfsFile *file);             // 0 / -1
int64_t readFs(VfsFile *file, void *buffer, uint64_t length);           // bytes, or -1
int64_t writeFs(VfsFile *file, const void *buffer, uint64_t length);    // bytes, or -1
int64_t seekFs(VfsFile *file, int64_t offset, int whence);              // new position, or -1
int     fsyncFs(VfsFile *file);                                         // 0 / -1
int     closeFs(VfsFile *file);                                         // 0 / -1 (commit error at close)

// internal (backend support): the synthetic "/" reader in cellfs.c asks the
// router for the next virtual mount. advances *cursor; 1 if one was written to
// nameOut, 0 when the registry is exhausted. not for general callers.
int getNextRootMount(int *cursor, char *nameOut, int nameCapacity);

// ---- cross-backend file & tree operations ----------------------------------
// Composed from the primitives above (implemented in vfs.c), so they work
// identically on every backend. allocation-free and prx-safe.

int      isDir(const char *path);
int      fileExists(const char *path);
int      makeDir(const char *path);                  // 0 if created or already present
int      deleteFile(const char *path);               // idempotent: 0 if already absent
void     syncPath(const char *path);                 // durability flush for path's volume

// reads up to cap-1 bytes + NUL. returns bytes read (0 = empty file), or -1.
int      readFile(const char *path, char *buf, int cap);
int      writeFile(const char *path, const char *data, uint64_t len);

// copyFile/copyTree/moveTree take caller scratch buf/bufSize (e.g. 64 KB) for
// the file payload so they stay allocation-free. 0 ok, -1 error.
int      copyFile(const char *src, const char *dst, void *buf, int bufSize);
int      copyTree(const char *src, const char *dst, void *buf, int bufSize);
int      moveTree(const char *src, const char *dst, void *buf, int bufSize);
// recursive delete; adds each removed regular file's size into *bytesFreed (NULL to ignore).
int      deleteTree(const char *path, uint64_t *bytesFreed);

// progress-reporting, cancellable cousins: onBytes(n) per chunk/file, cancelled()
// polled between entries; either callback may be NULL.
//   measureTree        - sum of regular-file bytes under path (partial on cancel).
//   copyTreeProgress   - 0 ok, -1 error, 1 cancelled. reports bytes per chunk.
//   deleteTreeProgress - 0 ok, -1 error, 1 cancelled. reports bytes per file.
uint64_t measureTree(const char *path, int (*cancelled)(void));
int      copyTreeProgress(const char *src, const char *dst, void *buf, int bufSize,
                          void (*onBytes)(uint64_t), int (*cancelled)(void));
int      deleteTreeProgress(const char *path,
                            void (*onBytes)(uint64_t), int (*cancelled)(void));

// merges src into dst (creating/descending as needed). for regular-file leaves
// that already exist at dst, replaceExisting != 0 overwrites, == 0 keeps the
// destination (a skipped file's bytes are still reported via onBytes). 0 ok, -1
// error, 1 cancelled.
int      mergeTreeProgress(const char *src, const char *dst, int replaceExisting,
                           void *buf, int bufSize,
                           void (*onBytes)(uint64_t), int (*cancelled)(void));

// counts regular-file leaves of src that would land on an existing entry if src
// were merged into dst, stopping once cap is reached (pass a small cap when only
// none/one/many matters).
int      countTreeConflicts(const char *src, const char *dst, int cap);
