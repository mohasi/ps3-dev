#pragma once
//
// cellfs.h - the Sony-kernel filesystem backend, a peer of exfat.c.
//
// This backend delegates to the PS3 kernel's own filesystem syscalls
// (cellFsOpen/Read/Write/...), which cover the internal HDD (Sony's
// UFS-derived format), kernel-mounted FAT32 USB and /dev_flash alike. It is
// named for the API it wraps, not an on-disk format, because the kernel - not
// us - owns the format. (Contrast exfat.c, named for a format we implement.)
//
// cellFs is the VFS default route: resolvePath() hands any path that matches no
// virtual mount to CELLFS_OPS verbatim, and the synthetic "/" listing uses
// ROOT_OPS. Both vtables are defined here and referenced by the router in vfs.c;
// this is the only translation unit in the library that calls cellFs*.
//
#include "vfs.h"

// default backend for unmatched (cellFs) paths.
extern const VfsOps CELLFS_OPS;

// synthetic root ("/") listing: real cellFs root devices, then the virtual
// mounts (pulled from the router via getNextRootMount). reuses CELLFS_OPS for
// every non-directory operation.
extern const VfsOps ROOT_OPS;
