#pragma once

// The .torrent file itself: what to ask a tracker for, and what the content is made of.
//
// The piece hashes are left where they arrived rather than copied, so the document the caller
// fetched has to stay put for as long as the metadata is used.
//
// TorrentMeta is around 60 KB because of the file list, so keep one rather than passing it about by
// value, and do not put one on a thread's stack.

#include <stdint.h>

#include "sha1.h"

#define TRACKER_URL_MAX      128
#define TRACKER_MAX            8
#define TORRENT_NAME_MAX     160
#define TORRENT_PATH_MAX     192
#define TORRENT_FILE_LIST_MAX 256   // a torrent with more files than this is refused rather than half read

typedef struct {
   char    path[TORRENT_PATH_MAX];   // below the torrent's own folder, with '/' between the parts
   int64_t length;
} TorrentFile;

typedef struct {
   char           name[TORRENT_NAME_MAX];
   char           trackers[TRACKER_MAX][TRACKER_URL_MAX];
   int            trackerCount;
   int64_t        totalLength;
   TorrentFile    files[TORRENT_FILE_LIST_MAX];   // one entry even for a torrent of a single file
   int            fileCount;
   int            pieceLength;
   int            pieceCount;
   const uint8_t *pieceHashes;   // SHA1_LENGTH bytes per piece, inside the document
   uint8_t        infoHash[SHA1_LENGTH];
} TorrentMeta;

// Read a .torrent that is already in memory. 0 when it holds everything a transfer needs, -1 when it
// does not. The info hash is taken over the exact bytes of the info dictionary as they arrived.
int readTorrentFile(const uint8_t *document, int length, TorrentMeta *meta);

// The same, for a bare description with no file around it, which is what a peer sends when a magnet
// link is all we started with. Trackers come from the magnet in that case, not from here.
int readTorrentInfo(const uint8_t *document, int length, TorrentMeta *meta);

// how long a piece is: every piece but the last is a full one. -1 for an index the torrent has not.
int getPieceLength(const TorrentMeta *meta, int pieceIndex);

// Fetch a .torrent over whichever transport the app installed, then read it. The document is left in
// the caller's buffer because the piece hashes point into it.
int loadTorrentFile(const char *url, uint8_t *document, int capacity, int *length, TorrentMeta *meta);
