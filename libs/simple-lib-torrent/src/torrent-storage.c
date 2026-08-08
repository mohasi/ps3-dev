// torrent-storage - putting pieces on disk, and knowing what is already there.

#include "torrent-storage.h"

#include "dbg.h"
#include "sha1.h"
#include "string-utilities.h"
#include "vfs.h"

#define TAG "[bt] "

#define INCOMPLETE_FOLDER "incomplete"

// Make the folders a torrent's own file sits in. Only the parts below contentPath are created: the
// folders above it are the console's and are not ours to make.
static int makeDirsFor(const char *contentPath, const char *filePath)
{
   char folder[MAX_PATH_LEN];
   joinPath(folder, sizeof folder, contentPath, filePath);

   for (int index = getStrLen(contentPath) + 1; folder[index]; index++) {
      if (folder[index] != '/') continue;

      folder[index] = 0;
      if (makeDir(folder) != 0) {
         logTrace(TAG "storage: %s could not be made\n", folder);
         return -1;
      }
      folder[index] = '/';
   }

   return 0;
}

// where in the torrent's single long run of bytes each file sits
static int64_t getFileStart(const TorrentMeta *meta, int fileIndex)
{
   int64_t start = 0;
   for (int index = 0; index < fileIndex; index++) start += meta->files[index].length;
   return start;
}

// Walk the files a piece covers, handing each one the part of the piece that belongs to it. reading
// says whether to fill data from disk or write it there. Returns 0, or -1.
static int visitPiece(const TorrentMeta *meta, const char *contentPath, int pieceIndex, uint8_t *data, int length,
                      int reading)
{
   int64_t pieceStart = (int64_t)pieceIndex * meta->pieceLength;
   int64_t pieceEnd = pieceStart + length;

   for (int index = 0; index < meta->fileCount; index++) {
      int64_t fileStart = getFileStart(meta, index);
      int64_t fileEnd = fileStart + meta->files[index].length;
      if (fileEnd <= pieceStart || fileStart >= pieceEnd) continue;

      int64_t from = pieceStart > fileStart ? pieceStart : fileStart;
      int64_t to = pieceEnd < fileEnd ? pieceEnd : fileEnd;
      int take = (int)(to - from);

      char path[MAX_PATH_LEN];
      joinPath(path, sizeof path, contentPath, meta->files[index].path);
      if (!reading && makeDirsFor(contentPath, meta->files[index].path) != 0) return -1;

      VfsFile file;
      int flags = reading ? VFS_O_RDONLY : (VFS_O_WRONLY | VFS_O_CREAT);
      if (openFs(path, flags, &file) != 0) {
         if (!reading) logTrace(TAG "storage: %s could not be opened to write\n", path);
         return -1;
      }

      int done = 0;
      int64_t moved = -1;
      if (seekFs(&file, from - fileStart, VFS_SEEK_SET) >= 0) {
         uint8_t *part = data + (from - pieceStart);
         moved = reading ? readFs(&file, part, take) : writeFs(&file, part, take);
         done = moved == take;
      }

      closeFs(&file);
      if (!done) {
         if (!reading) logTrace(TAG "storage: %s took %lld of %d bytes\n", path, (long long)moved, take);
         return -1;
      }
   }

   return 0;
}

// where a torrent that finished on an earlier run already sits, if it does
static int isAlreadyFinished(const TorrentMeta *meta, const char *downloadsDirectory, char *contentPath, int capacity)
{
   char path[MAX_PATH_LEN];

   if (meta->fileCount == 1) {
      joinPath(path, sizeof path, downloadsDirectory, meta->files[0].path);
      if (!fileExists(path)) return 0;

      strCopy(contentPath, capacity, downloadsDirectory);
      return 1;
   }

   joinPath(path, sizeof path, downloadsDirectory, meta->name);
   if (!isDir(path)) return 0;

   strCopy(contentPath, capacity, path);
   return 1;
}

int prepareTorrentStorage(const TorrentMeta *meta, const char *downloadsDirectory, char *contentPath, int capacity)
{
   char incomplete[MAX_PATH_LEN];
   joinPath(incomplete, sizeof incomplete, downloadsDirectory, INCOMPLETE_FOLDER);

   // one we already have: work against where it sits rather than downloading it beside itself
   if (isAlreadyFinished(meta, downloadsDirectory, contentPath, capacity)) {
      logTrace(TAG "storage: %s is already in %s\n", meta->name, downloadsDirectory);
      return 0;
   }

   if (makeDir(downloadsDirectory) != 0 || makeDir(incomplete) != 0) {
      logTrace(TAG "storage: %s could not be made\n", incomplete);
      return -1;
   }

   // one file goes straight into the folder; several get a folder named after the torrent
   if (meta->fileCount == 1) {
      strCopy(contentPath, capacity, incomplete);
      return 0;
   }

   joinPath(contentPath, capacity, incomplete, meta->name);
   if (makeDir(contentPath) != 0) {
      logTrace(TAG "storage: %s could not be made\n", contentPath);
      return -1;
   }

   return 0;
}

int writeTorrentPiece(const TorrentMeta *meta, const char *contentPath, int pieceIndex, const uint8_t *data,
                      int length)
{
   return visitPiece(meta, contentPath, pieceIndex, (uint8_t *)data, length, 0);
}

int isPieceOnDisk(const TorrentMeta *meta, const char *contentPath, int pieceIndex, uint8_t *scratch, int capacity)
{
   int length = getPieceLength(meta, pieceIndex);
   if (length <= 0 || length > capacity) return 0;
   if (visitPiece(meta, contentPath, pieceIndex, scratch, length, 1) != 0) return 0;

   uint8_t hash[SHA1_LENGTH];
   hashSha1(hash, scratch, length);
   for (int index = 0; index < SHA1_LENGTH; index++)
      if (hash[index] != meta->pieceHashes[pieceIndex * SHA1_LENGTH + index]) return 0;

   return 1;
}

int finishTorrentStorage(const TorrentMeta *meta, const char *downloadsDirectory, const char *contentPath)
{
   char from[MAX_PATH_LEN], to[MAX_PATH_LEN], incomplete[MAX_PATH_LEN];

   joinPath(incomplete, sizeof incomplete, downloadsDirectory, INCOMPLETE_FOLDER);
   joinPath(from, sizeof from, incomplete, meta->name);
   joinPath(to, sizeof to, downloadsDirectory, meta->name);

   // written straight into the downloads folder, because an earlier run had already finished it
   if (!startsWith(contentPath, incomplete)) return 0;

   syncPath(from);
   if (renamePath(from, to) != 0) {
      logError(TAG "storage: a finished torrent could not be moved out of %s\n", INCOMPLETE_FOLDER);
      return -1;
   }

   logTrace(TAG "storage: %s is finished and now in %s\n", meta->name, downloadsDirectory);
   return 0;
}
