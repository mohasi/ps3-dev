#pragma once

// stat-tree <root>
//
// recursive filesystem snapshot for before/after install diffs. walks
// <root> iteratively (no recursion) and writes one line per entry to
// /dev_hdd0/tmp/stat-tree.txt (overwritten on each call):
//
//   <kind>\t<size>\t<mtime>\t<sha1>\t<path>\n
//
// kind is 'f' (regular file), 'd' (directory, incl. empty), or 'l'
// (symlink - never followed; size/mtime 0). sha1 is 40 lowercase hex
// for regular files up to STAT_TREE_HASH_MAX (256 KiB); larger files,
// dirs, symlinks, and any open/read/hash error all emit 40 zeros. the
// cap exists because sha1'ing multi-MB game data dwarfs everything
// else; install diffs care about config/manifest changes (PRX, SFO,
// DB, txt) which all fit well under the cap, and large files still
// diff on size+mtime. on the wire we reply with just
// "OK files=<n> dirs=<n> -> <out path>" - the data lives in the file
// on the ps3, pulled later with pull-file.
//
// memory: one 64 KiB heap allocation via sysMemAllocate (SYS_PAGE_64K)
// at command entry, freed on every exit path. nothing retained between
// calls, no per-node allocations, no static buffers. the ctx carves the
// 64 KiB into: path (2 KiB), write buf (16 KiB), read buf (32 KiB),
// 64-frame dfs stack, one reused CellFsDirent, sha1 state, slack.
//
// stability: favours not wedging vsh over throughput. yields after
// every directory close and after every read buffer hashed. errors
// (stat/open/read/recurse-depth/path-overflow) silently skip the
// offending entry and continue - never aborts the walk.

#include "cmd-common.h"
#include "fileio.h"

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include <sys/fs_external.h>

#include "sha1.h"

#define STAT_TREE_OUT_PATH    "/dev_hdd0/tmp/stat-tree.txt"
#define STAT_TREE_HASH_MAX    (256 * 1024)
#define STAT_TREE_PATH_MAX    2048
#define STAT_TREE_WRITE_MAX   (16 * 1024)
#define STAT_TREE_READ_MAX    (32 * 1024)
#define STAT_TREE_STACK_MAX   64
#define STAT_TREE_ARENA_BYTES (64u * 1024u)  // sys_memory_allocate needs a 64K-aligned size for SYS_PAGE_64K

typedef struct {
   int      fd;
   uint32_t pathLen;
} StatFrame;

typedef struct {
   char         path[STAT_TREE_PATH_MAX];
   char         writeBuf[STAT_TREE_WRITE_MAX];
   uint32_t     writeLen;
   uint8_t      readBuf[STAT_TREE_READ_MAX];
   StatFrame    stack[STAT_TREE_STACK_MAX];
   int          depth;
   CellFsDirent ent;
   Sha1State    sha;
   int          outFd;
   uint32_t     files;
   uint32_t     dirs;
} StatTreeCtx;

static const char STAT_TREE_ZERO_SHA1[41] =
   "0000000000000000000000000000000000000000";

static const char STAT_TREE_HEX[17] = "0123456789abcdef";

static int statTreeFlush(StatTreeCtx *c)
{
   if (c->writeLen == 0) return 0;
   uint64_t written = 0;
   int rc = (int)cellFsWrite(c->outFd, c->writeBuf, c->writeLen, &written);
   c->writeLen = 0;
   return (rc == CELL_FS_SUCCEEDED) ? 0 : -1;
}

static void statTreeReserve(StatTreeCtx *c, uint32_t need)
{
   if (c->writeLen + need > STAT_TREE_WRITE_MAX) statTreeFlush(c);
}

static void statTreeAppendBytes(StatTreeCtx *c, const char *src, uint32_t len)
{
   while (len > 0) {
      uint32_t space = STAT_TREE_WRITE_MAX - c->writeLen;
      if (space == 0) { statTreeFlush(c); space = STAT_TREE_WRITE_MAX; }
      uint32_t take = (len < space) ? len : space;
      for (uint32_t i = 0; i < take; i++) c->writeBuf[c->writeLen + i] = src[i];
      c->writeLen += take;
      src += take;
      len -= take;
   }
}

static void statTreeAppendChar(StatTreeCtx *c, char ch)
{
   statTreeReserve(c, 1);
   c->writeBuf[c->writeLen++] = ch;
}

static void statTreeAppendU64(StatTreeCtx *c, uint64_t v)
{
   char tmp[24];
   int n = 0;
   if (v == 0) {
      tmp[n++] = '0';
   } else {
      while (v > 0) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
   }
   statTreeReserve(c, (uint32_t)n);
   while (n > 0) c->writeBuf[c->writeLen++] = tmp[--n];
}

// hash a single regular file. fills out[] with 40 hex chars on success,
// 40 zeros on error or files > STAT_TREE_HASH_MAX. yields after every
// read so the kernel scheduler stays responsive on big files.
static void statTreeHashFile(StatTreeCtx *c, const char *path, uint64_t size, char out[40])
{
   for (int i = 0; i < 40; i++) out[i] = '0';
   if (size > STAT_TREE_HASH_MAX) return;

   int fd;
   if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return;

   initSha1(&c->sha);
   uint64_t remaining = size;
   int ok = 1;
   while (remaining > 0) {
      uint64_t want = (remaining < STAT_TREE_READ_MAX) ? remaining : STAT_TREE_READ_MAX;
      uint64_t got = 0;
      if (cellFsRead(fd, c->readBuf, want, &got) != CELL_FS_SUCCEEDED || got == 0) {
         ok = 0;
         break;
      }
      updateSha1(&c->sha, c->readBuf, (int)got);
      remaining -= got;
      yieldThread();
   }
   cellFsClose(fd);

   if (!ok) return;

   uint8_t digest[20];
   finalizeSha1(&c->sha, digest);
   for (int i = 0; i < 20; i++) {
      out[i * 2 + 0] = STAT_TREE_HEX[(digest[i] >> 4) & 0xF];
      out[i * 2 + 1] = STAT_TREE_HEX[ digest[i]       & 0xF];
   }
}

static void statTreeEmit(StatTreeCtx *c, char kind, uint64_t size, int64_t mtime, const char *sha40)
{
   statTreeAppendChar(c, kind);
   statTreeAppendChar(c, '\t');
   statTreeAppendU64(c, size);
   statTreeAppendChar(c, '\t');
   statTreeAppendU64(c, (uint64_t)mtime);
   statTreeAppendChar(c, '\t');
   statTreeAppendBytes(c, sha40, 40);
   statTreeAppendChar(c, '\t');
   statTreeAppendBytes(c, c->path, (uint32_t)getStrLen(c->path));
   statTreeAppendChar(c, '\n');
}

// process one child of the directory at the top of the stack. mutates
// ctx->path to "<parent>/<name>", emits its line, and for a directory
// also pushes a new frame for the main loop to descend into.
static void statTreeVisitChild(StatTreeCtx *c, const char *name, uint8_t d_type)
{
   uint32_t parentLen = c->stack[c->depth - 1].pathLen;
   uint32_t nameLen   = (uint32_t)getStrLen(name);

   if (parentLen + 1 + nameLen + 1 > STAT_TREE_PATH_MAX) return;

   c->path[parentLen] = '/';
   for (uint32_t i = 0; i < nameLen; i++) c->path[parentLen + 1 + i] = name[i];
   uint32_t childLen = parentLen + 1 + nameLen;
   c->path[childLen] = '\0';

   if (d_type == CELL_FS_TYPE_SYMLINK) {
      statTreeEmit(c, 'l', 0, 0, STAT_TREE_ZERO_SHA1);
      c->path[parentLen] = '\0';
      return;
   }

   if (d_type == CELL_FS_TYPE_DIRECTORY) {
      uint64_t size  = 0;
      int64_t  mtime = 0;
      CellFsStat st;
      if (cellFsStat(c->path, &st) == CELL_FS_SUCCEEDED) {
         size  = st.st_size;
         mtime = (int64_t)st.st_mtime;
      }
      statTreeEmit(c, 'd', size, mtime, STAT_TREE_ZERO_SHA1);
      c->dirs++;

      if (c->depth >= STAT_TREE_STACK_MAX) {
         c->path[parentLen] = '\0';
         return;
      }
      int dfd;
      if (cellFsOpendir(c->path, &dfd) != CELL_FS_SUCCEEDED) {
         c->path[parentLen] = '\0';
         return;
      }
      c->stack[c->depth].fd      = dfd;
      c->stack[c->depth].pathLen = childLen;
      c->depth++;
      return;
   }

   if (d_type == CELL_FS_TYPE_REGULAR) {
      uint64_t size  = 0;
      int64_t  mtime = 0;
      CellFsStat st;
      if (cellFsStat(c->path, &st) == CELL_FS_SUCCEEDED) {
         size  = st.st_size;
         mtime = (int64_t)st.st_mtime;
      }
      char sha[40];
      statTreeHashFile(c, c->path, size, sha);
      statTreeEmit(c, 'f', size, mtime, sha);
      c->files++;
      c->path[parentLen] = '\0';
      return;
   }

   c->path[parentLen] = '\0';
}

static void statTreeWalk(StatTreeCtx *c)
{
   while (c->depth > 0) {
      StatFrame *top = &c->stack[c->depth - 1];

      uint64_t nread = 0;
      if (cellFsReaddir(top->fd, &c->ent, &nread) != CELL_FS_SUCCEEDED || nread == 0) {
         cellFsClosedir(top->fd);
         c->depth--;
         c->path[top->pathLen] = '\0';
         yieldThread();
         continue;
      }

      const char *name = c->ent.d_name;
      if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
         continue;
      }
      statTreeVisitChild(c, name, c->ent.d_type);
   }
}

static void cmdStatTree(int cli, const char *args)
{
   char root[FILE_PATH_MAX];
   if (!parsePath(args, root, sizeof root)) {
      sendReply(cli, SDB_ERR, "usage: stat-tree <root>");
      return;
   }

   CellFsStat rst;
   if (cellFsStat(root, &rst) != CELL_FS_SUCCEEDED ||
       (rst.st_mode & CELL_FS_S_IFMT) != CELL_FS_S_IFDIR) {
      sendReply(cli, SDB_ERR, "root not a directory");
      return;
   }

   uint32_t ctxAddr = 0;
   int32_t  allocRc = sysMemAllocate(STAT_TREE_ARENA_BYTES, SYS_PAGE_64K, &ctxAddr);
   if (allocRc < 0 || ctxAddr == 0 || sizeof(StatTreeCtx) > STAT_TREE_ARENA_BYTES) {
      if (ctxAddr) sysMemFree(ctxAddr);
      sendErrRc(cli, "alloc failed", allocRc);
      return;
   }
   StatTreeCtx *c = (StatTreeCtx *)(uintptr_t)ctxAddr;
   c->writeLen = 0;
   c->depth    = 0;
   c->outFd    = -1;
   c->files    = 0;
   c->dirs     = 0;

   if (cellFsOpen(STAT_TREE_OUT_PATH,
                  CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                  &c->outFd, NULL, 0) != CELL_FS_SUCCEEDED) {
      sysMemFree(ctxAddr);
      sendReply(cli, SDB_ERR, "open output failed");
      return;
   }

   uint32_t rlen = (uint32_t)getStrLen(root);
   while (rlen > 1 && root[rlen - 1] == '/') rlen--;
   if (rlen >= STAT_TREE_PATH_MAX) {
      cellFsClose(c->outFd);
      sysMemFree(ctxAddr);
      sendReply(cli, SDB_ERR, "root path too long");
      return;
   }
   for (uint32_t i = 0; i < rlen; i++) c->path[i] = root[i];
   c->path[rlen] = '\0';

   int rfd;
   if (cellFsOpendir(c->path, &rfd) != CELL_FS_SUCCEEDED) {
      cellFsClose(c->outFd);
      sysMemFree(ctxAddr);
      sendReply(cli, SDB_ERR, "opendir root failed");
      return;
   }

   statTreeEmit(c, 'd', rst.st_size, (int64_t)rst.st_mtime, STAT_TREE_ZERO_SHA1);
   c->dirs++;

   c->stack[0].fd      = rfd;
   c->stack[0].pathLen = rlen;
   c->depth            = 1;

   statTreeWalk(c);
   statTreeFlush(c);
   cellFsClose(c->outFd);

   uint32_t files = c->files;
   uint32_t dirs  = c->dirs;
   sysMemFree(ctxAddr);

   char reply[96];
   snprintf(reply, sizeof reply, "files=%u dirs=%u -> %s",
            (unsigned)files, (unsigned)dirs, STAT_TREE_OUT_PATH);
   sendReply(cli, SDB_OK, reply);
}
