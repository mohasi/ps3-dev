#pragma once

// generic ps3 file io primitives over a tcp socket. no protocol awareness —
// callers (server.h) own the wire framing. used by both raw file commands
// (pull-file / push-file) and higher-level wrappers (plugin.h, pkg.h).
// note: non-streaming primitives (readFile/writeFile/deleteFile/...) live in
// the shared prx lib's vfs.h — this header only adds the socket-coupled ones.
// all filesystem access routes through the VFS (backend-agnostic).

#include <stdint.h>
#include <sys/socket.h>
#include "vfs.h"
#include "printf.h"

#define FILE_PATH_MAX  512
#define FILE_CHUNK     4096

// stream `size` bytes from socket `cli` into `path` (truncating).
// caller is responsible for ensuring the parent directory exists.
// returns 0 on success, -1 on any io failure.
static int recvFile(int cli, const char *path, uint32_t size)
{
   VfsFile f;
   if (openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &f) != 0) return -1;

   static char chunk[FILE_CHUNK];
   uint32_t remaining = size;
   while (remaining > 0) {
      int want = (int)(remaining < sizeof chunk ? remaining : sizeof chunk);
      int got = recv(cli, chunk, want, 0);
      if (got <= 0) { closeFs(&f); return -1; }
      if (writeFs(&f, chunk, (uint64_t)got) != (int64_t)got) {
         closeFs(&f);
         return -1;
      }
      remaining -= (uint32_t)got;
   }
   // fold the close: a deferred commit error means the upload isn't durable.
   return closeFs(&f) == 0 ? 0 : -1;
}

// resolve [offset, offset+length) against `path`. length=0 means "to end".
// returns the actual byte count to send, or -1 if the file is missing /
// the offset is past EOF. lets callers size a "<n>"-style header before
// committing to streaming bytes.
static int64_t fileWindowSize(const char *path, uint64_t offset, uint64_t length)
{
   VfsStat st;
   if (statPath(path, &st) != 0) return -1;
   uint64_t total = st.size;
   if (offset > total) return -1;
   uint64_t avail = total - offset;
   if (length == 0 || length > avail) length = avail;
   return (int64_t)length;
}

// stream `length` bytes of `path` starting at `offset` to socket `cli`.
// returns 0 on success, -1 on io failure (caller should already have
// committed to the response header by this point).
static int sendFileWindow(int cli, const char *path, uint64_t offset, uint64_t length)
{
   VfsFile f;
   if (openFs(path, VFS_O_RDONLY, &f) != 0) return -1;

   if (offset > 0) {
      if (seekFs(&f, (int64_t)offset, VFS_SEEK_SET) < 0) {
         closeFs(&f);
         return -1;
      }
   }

   static char chunk[FILE_CHUNK];
   uint64_t remaining = length;
   while (remaining > 0) {
      uint64_t want = remaining < sizeof chunk ? remaining : sizeof chunk;
      int64_t got = readFs(&f, chunk, want);
      if (got <= 0) {
         closeFs(&f);
         return -1;
      }
      const char *p = chunk;
      uint64_t left = (uint64_t)got;
      while (left > 0) {
         int n = send(cli, p, (int)left, 0);
         if (n <= 0) { closeFs(&f); return -1; }
         p += n;
         left -= (uint64_t)n;
      }
      remaining -= (uint64_t)got;
   }
   closeFs(&f);
   return 0;
}

// list one directory into `out`, one entry per line:
//   "<kind>\t<size>\t<mtime>\t<name>\n"
// where kind is 'f' (regular), 'd' (directory), or '?' (stat failed).
// returns total bytes written on success (0 == empty dir), or -1 on failure
// (open or buffer-too-small). entries are stat'd individually so size/mtime
// reflect the underlying inode, matching what an ftp ls would show.
static int listDir(const char *dir, char *out, int cap)
{
   VfsDir d;
   if (openDir(dir, &d) != 0) return -1;

   int written = 0;
   char name[256];
   VfsEntryType type = VFS_ENTRY_OTHER;
   while (readDir(&d, name, sizeof name, &type) == 1) {   // VFS filters "." / ".."
      char path[FILE_PATH_MAX];
      if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) continue;

      // kind from the dirent type (not a follow-stat): 'd'/'f', '?' for symlink/other -
      // preserves the documented f/d/? set (a symlink is not reported as a regular file).
      char kind = (type == VFS_ENTRY_DIR) ? 'd' : (type == VFS_ENTRY_FILE) ? 'f' : '?';
      uint64_t size = 0;
      int64_t  mtime = 0;
      VfsStat st;
      if (statPath(path, &st) == 0) {
         size  = st.size;
         mtime = (int64_t)st.mtime;
      }

      int n = snprintf(out + written, cap - written,
                       "%c\t%llu\t%lld\t%s\n",
                       kind, (unsigned long long)size, (long long)mtime, name);
      if (n < 0 || n >= cap - written) { closeDir(&d); return -1; }
      written += n;
   }
   closeDir(&d);
   return written;
}
