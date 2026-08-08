#pragma once

// A magnet link, which is what most sites hand out instead of a .torrent file. It carries the hash
// that names the torrent, usually a display name, and usually a list of trackers. It does not carry
// the file list or the piece hashes, so those have to be asked of a peer afterwards.
//
//   magnet:?xt=urn:btih:<hash>&dn=<name>&tr=<tracker>&tr=<tracker>

#include <stdint.h>

#include "torrent-file.h"

typedef struct {
   uint8_t infoHash[SHA1_LENGTH];
   char    name[TORRENT_NAME_MAX];
   char    trackers[TRACKER_MAX][TRACKER_URL_MAX];
   int     trackerCount;
} MagnetLink;

int isMagnetLink(const char *text);

// Read one. Returns 0, or -1 when there is no usable hash in it. The hash may be written as forty
// hex characters or as thirty two base32 ones; both are seen in the wild.
int readMagnetLink(MagnetLink *magnet, const char *uri);
