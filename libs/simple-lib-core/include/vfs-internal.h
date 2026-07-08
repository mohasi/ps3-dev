//
// vfs-internal.h - shared between vfs.c (router + mount registry) and vfs-init.c
// (backend bringup + USB hotplug). NOT part of the public VFS API (vfs.h).
//
// The split exists so a consumer that only routes paths (e.g. a plugin calling
// openDir on /dev_hdd0) pulls vfs.o alone and never drags in the exFAT/NTFS
// drivers, which only vfs-init.c names. This toolchain's linker can't strip
// unreachable code, so the only lever is which object files get pulled - keeping
// initVfs (and its initExfat/initNtfs calls) in its own TU is that lever.
//
// vfs.c owns the registry and its lock; this header lets vfs-init.c serialize its
// own poll state against the same single lock without reaching into mounts[].
//
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

void ensureMountsLock(void);   // create the registry lock once; safe to call repeatedly
void lockMounts(void);         // no-op until ensureMountsLock has run (bootstrap-safe)
void unlockMounts(void);
void clearMounts(void);        // drop every mount (mountCount = 0) under the lock

// registers the optional http(s):// backend (http-fs). requires vfs.h included
// first for VfsOps. NULL unregisters. only the app that links http-fs calls this.
void setUrlVfsBackend(const VfsOps *ops);

#endif
