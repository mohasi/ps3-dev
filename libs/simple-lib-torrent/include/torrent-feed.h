#pragma once

// One torrent index feed, fetched over the tunnel and turned into a list.
//
// An RSS feed is one <item> per torrent. Nyaa's carries the info hash alongside the title, which is
// all a client needs to start: the .torrent file itself can be found from the hash later.

#include <stdint.h>

#include "torrent-source.h"   // how a source's answer is laid out is the source's own business

#define TORRENT_TITLE_MAX 160
#define TORRENT_URL_MAX   128
#define TORRENT_SIZE_MAX   24   // as the feed words it, e.g. "275.3 MiB"
#define INFO_HASH_LENGTH   20

typedef struct {
   char    title[TORRENT_TITLE_MAX];
   char    torrentUrl[TORRENT_URL_MAX];
   char    size[TORRENT_SIZE_MAX];
   uint8_t infoHash[INFO_HASH_LENGTH];
   int     hasInfoHash;
   int     seeders;
   int     leechers;
   int     category;   // the number the site files it under, 0 when it does not say
} TorrentItem;

// Fetch a source's results and fill items, returning how many were found, or -1 when they could not
// be fetched. How the reply is read comes from the source. Items past capacity are ignored.
int loadTorrentResults(const TorrentSource *source, const char *url, TorrentItem *items, int capacity);

// the parsing on its own, so each can be checked without a network
int parseTorrentFeed(const char *feed, int length, TorrentItem *items, int capacity);
int parseTorrentJson(const TorrentSource *source, const char *text, int length, TorrentItem *items,
                     int capacity);
