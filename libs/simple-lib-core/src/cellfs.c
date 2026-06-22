//
// cellfs.c - the Sony-kernel filesystem backend (see cellfs.h).
//
// Thin wrappers over the cellFs* syscalls the codebase used directly before the
// VFS existed, so HDD/FAT32 behaviour is byte-for-byte unchanged. This is the
// only file in the library permitted to call cellFs*; everything else routes
// through the VFS. Extracted out of vfs.c so the router stays a pure abstraction
// and every backend (cellfs, exfat, future ntfs) is one self-contained file.
//
#include "cellfs.h"

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include "string-utilities.h"   // strCopy

// "." / ".." filter shared by the file and root directory readers.
static int isDotEntry(const char *name)
{
   return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// section: cellFs backend - identity-path operations on the kernel filesystem.

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

static VfsEntryType cellFsEntryType(unsigned dType)
{
   if (dType == CELL_FS_TYPE_DIRECTORY) return VFS_ENTRY_DIR;
   if (dType == CELL_FS_TYPE_REGULAR)   return VFS_ENTRY_FILE;
   if (dType == CELL_FS_TYPE_SYMLINK)   return VFS_ENTRY_SYMLINK;
   return VFS_ENTRY_OTHER;
}

static int readDirCellFs(VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut)
{
   for (;;) {
      CellFsDirent entry;
      uint64_t bytesRead = 0;
      if (cellFsReaddir(dir->descriptor, &entry, &bytesRead) != CELL_FS_SUCCEEDED) return -1;   // I/O error
      if (bytesRead == 0) return 0;                                                              // end of dir
      if (isDotEntry(entry.d_name)) continue;
      strCopy(nameOut, nameCapacity, entry.d_name);
      if (typeOut) *typeOut = cellFsEntryType(entry.d_type);
      return 1;
   }
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
   int cellWhence;
   if      (whence == VFS_SEEK_SET) cellWhence = CELL_FS_SEEK_SET;
   else if (whence == VFS_SEEK_CUR) cellWhence = CELL_FS_SEEK_CUR;
   else if (whence == VFS_SEEK_END) cellWhence = CELL_FS_SEEK_END;
   else return -1;   // unknown whence: fail rather than silently seek absolute
   uint64_t position = 0;
   if (cellFsLseek(file->descriptor, offset, cellWhence, &position) != CELL_FS_SUCCEEDED) return -1;
   return (int64_t)position;
}

static int fsyncCellFs(VfsFile *file)
{
   (void)file;   // cellFs durability is path-level (syncDevice); see syncVfs
   return 0;
}

static int closeCellFs(VfsFile *file)
{
   int result = cellFsClose(file->descriptor);
   file->descriptor = -1;
   return result == CELL_FS_SUCCEEDED ? 0 : -1;
}

const VfsOps CELLFS_OPS = {
   statCellFs, renameCellFs, makeDirCellFs, removeFileCellFs, removeDirCellFs, getFreeCellFs,
   openDirCellFs, readDirCellFs, closeDirCellFs,
   openCellFs, readCellFs, writeCellFs, seekCellFs, fsyncCellFs, closeCellFs,
};

// section: synthetic "/" listing - cellFs root devices first, then the virtual
// mounts. descriptor holds the cellFs "/" handle while phase 1 runs; once
// drained it is -1 and nativeHandle carries the registry cursor for phase 2,
// which the router serves via getNextRootMount().

static int openDirRoot(const char *native, VfsDir *dir)
{
   (void)native;
   int descriptor;
   if (cellFsOpendir("/", &descriptor) != CELL_FS_SUCCEEDED) descriptor = -1;   // phase 2 still runs
   dir->descriptor   = descriptor;
   dir->nativeHandle = 0;
   return 0;
}

static int readDirRoot(VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut)
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

      int probeHandle;
      if (cellFsOpendir(probePath, &probeHandle) != CELL_FS_SUCCEEDED) continue;   // unenterable (devkit/system)
      cellFsClosedir(probeHandle);

      strCopy(nameOut, nameCapacity, entry.d_name);
      if (typeOut) *typeOut = VFS_ENTRY_DIR;
      return 1;
   }

   // phase 2: virtual NTFS/exFAT mounts - owned by the router (it holds the
   // registry + its lock), so ask it for the next one rather than reaching in.
   int cursor = (int)(intptr_t)dir->nativeHandle;
   int got = getNextRootMount(&cursor, nameOut, nameCapacity);
   dir->nativeHandle = (void *)(intptr_t)cursor;
   if (got) {
      if (typeOut) *typeOut = VFS_ENTRY_DIR;
      return 1;
   }
   return 0;
}

static void closeDirRoot(VfsDir *dir)
{
   if (dir->descriptor >= 0) { cellFsClosedir(dir->descriptor); dir->descriptor = -1; }
}

const VfsOps ROOT_OPS = {
   statCellFs, renameCellFs, makeDirCellFs, removeFileCellFs, removeDirCellFs, getFreeCellFs,
   openDirRoot, readDirRoot, closeDirRoot,
   openCellFs, readCellFs, writeCellFs, seekCellFs, fsyncCellFs, closeCellFs,
};
