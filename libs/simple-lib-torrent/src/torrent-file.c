// torrent-file - reading a .torrent into the handful of facts a transfer needs.

#include "torrent-file.h"

#include "bencode.h"
#include "dbg.h"
#include "http.h"
#include "path.h"   // isValidFileName, since a torrent says where its files go
#include "string-utilities.h"

#define TAG "[bt] "

static void addTracker(TorrentMeta *meta, const uint8_t *document, const BencodeValue *value)
{
   if (meta->trackerCount >= TRACKER_MAX || value->kind != BENCODE_STRING) return;

   char url[TRACKER_URL_MAX];
   if (copyBencodeString(document, value, url, sizeof url) != 0) return;

   for (int index = 0; index < meta->trackerCount; index++)
      if (strEq(meta->trackers[index], url)) return;

   strCopy(meta->trackers[meta->trackerCount++], TRACKER_URL_MAX, url);
}

// announce-list is a list of groups, each group a list of addresses for the same swarm
static void readTrackerList(TorrentMeta *meta, const uint8_t *document, int length, const BencodeValue *list)
{
   if (list->kind != BENCODE_LIST) return;

   for (int cursor = list->innerStart; cursor > 0;) {
      BencodeValue group;
      cursor = readBencodeItem(document, length, list, cursor, &group);
      if (cursor <= 0) return;

      if (group.kind == BENCODE_STRING) { addTracker(meta, document, &group); continue; }

      for (int inner = group.innerStart; inner > 0;) {
         BencodeValue tracker;
         inner = readBencodeItem(document, length, &group, inner, &tracker);
         if (inner <= 0) break;
         addTracker(meta, document, &tracker);
      }
   }
}

// A file's name is a list of parts, one per folder. A torrent is not to be trusted with where it
// writes, so every part is checked before it becomes a path: ".." or a separator in one would let a
// torrent put a file anywhere on the console.
static int readFilePath(const uint8_t *document, int length, const BencodeValue *entry, char *out, int capacity)
{
   BencodeValue parts;
   if (findBencodeMember(document, length, entry, "path", &parts) != 0 || parts.kind != BENCODE_LIST) return -1;

   int offset = 0;
   out[0] = 0;

   for (int cursor = parts.innerStart; cursor > 0;) {
      BencodeValue part;
      cursor = readBencodeItem(document, length, &parts, cursor, &part);
      if (cursor <= 0) break;

      char name[TORRENT_PATH_MAX];
      if (copyBencodeString(document, &part, name, sizeof name) != 0 || !isValidFileName(name)) return -1;

      if (offset > 0) appendStr(out, capacity, &offset, "/");
      appendStr(out, capacity, &offset, name);
      if (offset >= capacity - 1) return -1;

      out[offset] = 0;
   }

   return out[0] ? 0 : -1;
}

static int addFile(TorrentMeta *meta, const char *path, int64_t length)
{
   if (meta->fileCount >= TORRENT_FILE_LIST_MAX) return -1;

   strCopy(meta->files[meta->fileCount].path, TORRENT_PATH_MAX, path);
   meta->files[meta->fileCount].length = length;
   meta->fileCount++;
   meta->totalLength += length;
   return 0;
}

// one file has its length beside the name; several have a list of them instead
static int readContentSize(TorrentMeta *meta, const uint8_t *document, int length, const BencodeValue *info)
{
   BencodeValue single;
   if (findBencodeMember(document, length, info, "length", &single) == 0)
      return addFile(meta, meta->name, getBencodeInteger(document, &single));

   BencodeValue files;
   if (findBencodeMember(document, length, info, "files", &files) != 0 || files.kind != BENCODE_LIST) return -1;

   for (int cursor = files.innerStart; cursor > 0;) {
      BencodeValue entry;
      cursor = readBencodeItem(document, length, &files, cursor, &entry);
      if (cursor <= 0) break;

      BencodeValue size;
      char path[TORRENT_PATH_MAX];
      if (findBencodeMember(document, length, &entry, "length", &size) != 0) return -1;
      if (readFilePath(document, length, &entry, path, sizeof path) != 0) return -1;
      if (addFile(meta, path, getBencodeInteger(document, &size)) != 0) {
         logTrace(TAG "torrent: %s holds more than %d files\n", meta->name, TORRENT_FILE_LIST_MAX);
         return -1;
      }
   }

   return meta->fileCount > 0 ? 0 : -1;
}

// The part that describes the content, which is the same whether it came inside a .torrent file or
// straight from a peer. Trackers are not in here: they sit beside it, or come from a magnet link.
static int readInfoDictionary(const uint8_t *document, int length, const BencodeValue *info, TorrentMeta *meta)
{
   BencodeValue name, pieceLength, pieces;
   if (findBencodeMember(document, length, info, "name", &name) == 0)
      copyBencodeString(document, &name, meta->name, sizeof meta->name);

   if (!isValidFileName(meta->name)) {
      logError(TAG "torrent: its name is not one a folder can be called\n");
      return -1;
   }

   if (findBencodeMember(document, length, info, "piece length", &pieceLength) != 0) {
      logTrace(TAG "torrent: %s has no piece length\n", meta->name);
      return -1;
   }
   meta->pieceLength = (int)getBencodeInteger(document, &pieceLength);

   if (findBencodeMember(document, length, info, "pieces", &pieces) != 0) {
      logTrace(TAG "torrent: %s has no piece hashes\n", meta->name);
      return -1;
   }

   int hashesLength = getBencodeString(document, &pieces, &meta->pieceHashes);
   if (hashesLength <= 0 || hashesLength % SHA1_LENGTH != 0) {
      logTrace(TAG "torrent: %s has %d bytes of piece hashes, which is not a whole number of them\n", meta->name,
               hashesLength);
      return -1;
   }
   meta->pieceCount = hashesLength / SHA1_LENGTH;

   if (readContentSize(meta, document, length, info) != 0) {
      logTrace(TAG "torrent: %s does not say how large it is\n", meta->name);
      return -1;
   }

   if (meta->pieceLength <= 0) {
      logTrace(TAG "torrent: %s has a piece length of %d\n", meta->name, meta->pieceLength);
      return -1;
   }

   // the name of the torrent everywhere else: the hash of the info dictionary exactly as it arrived
   hashSha1(meta->infoHash, document + info->start, info->end - info->start);
   return 0;
}

int readTorrentInfo(const uint8_t *document, int length, TorrentMeta *meta)
{
   memSet(meta, 0, sizeof *meta);

   BencodeValue info;
   if (readBencode(document, length, 0, &info) < 0 || info.kind != BENCODE_DICTIONARY) {
      logError(TAG "torrent: what arrived is not a description\n");
      return -1;
   }

   return readInfoDictionary(document, length, &info, meta);
}

int readTorrentFile(const uint8_t *document, int length, TorrentMeta *meta)
{
   memSet(meta, 0, sizeof *meta);

   BencodeValue root, info;
   if (readBencode(document, length, 0, &root) < 0 || root.kind != BENCODE_DICTIONARY) {
      logError(TAG "torrent: the file is not bencode\n");
      return -1;
   }

   if (findBencodeMember(document, length, &root, "info", &info) != 0 || info.kind != BENCODE_DICTIONARY) {
      logError(TAG "torrent: the file has no info dictionary\n");
      return -1;
   }

   // where to ask for peers
   BencodeValue announce;
   if (findBencodeMember(document, length, &root, "announce", &announce) == 0) addTracker(meta, document, &announce);
   if (findBencodeMember(document, length, &root, "announce-list", &announce) == 0)
      readTrackerList(meta, document, length, &announce);

   if (readInfoDictionary(document, length, &info, meta) != 0) return -1;

   if (meta->trackerCount == 0) {
      logTrace(TAG "torrent: %s names no tracker\n", meta->name);
      return -1;
   }

   return 0;
}

int getPieceLength(const TorrentMeta *meta, int pieceIndex)
{
   if (pieceIndex < 0 || pieceIndex >= meta->pieceCount) return -1;

   int64_t left = meta->totalLength - (int64_t)pieceIndex * meta->pieceLength;
   return left < meta->pieceLength ? (int)left : meta->pieceLength;
}

int loadTorrentFile(const char *url, uint8_t *document, int capacity, int *length, TorrentMeta *meta)
{
   int status = 0;
   *length = 0;

   if (getHttp(url, (char *)document, capacity, length, &status) != 0) {
      logTrace(TAG "torrent: %s could not be fetched\n", url);
      return -1;
   }

   if (status != 200) {
      logTrace(TAG "torrent: %s answered %d\n", url, status);
      return -1;
   }

   return readTorrentFile(document, *length, meta);
}
