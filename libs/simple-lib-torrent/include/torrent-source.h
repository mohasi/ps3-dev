#pragma once

// Where torrents are searched for. None are built in: a source is a small text file, and a folder of
// them is the whole list. Adding a site is copying a file in, removing one is deleting it, and
// turning one off is moving it into the "disabled" folder beside them.
//
//   name    = Example
//   search  = https://example.org/rss?q={query}
//   torrent = https://example.org/download/{hash}.torrent
//   format  = rss
//
// A source has to have a search address, and one without it is skipped: a site that cannot be given
// words to look for could only ever show the same few newest torrents. {query} is replaced by what
// was typed, escaped for a URL. torrent says where the torrent file itself lives, with {hash}
// standing for the one in the results; a site whose results link straight to the file does not need
// the line. format says how to read the reply, and a source asking for one this build cannot read is
// skipped rather than guessed at. name falls back to the file's own name.
//
// A site that answers with data rather than a feed says which member holds what, and what trackers
// to put in the magnet built from a bare hash:
//
//   format  = json
//   list    = torrents
//   title   = name
//   hash    = infohash
//   size    = size_bytes
//   seeds   = seeders
//   peers   = leechers
//   category = category            which member holds the number the site files it under
//   adultcategories = 500-599      the run of those numbers that means pornography
//   tracker = udp://tracker.opentrackr.org:1337/announce

#include <stdint.h>

#define SOURCE_NAME_MAX   32
#define SOURCE_URL_MAX   256
#define SOURCE_QUERY_MAX 128
#define SOURCE_FIELD_MAX  24
#define SOURCE_TRACKER_MAX 4

typedef enum {
   SOURCE_FORMAT_RSS,
   SOURCE_FORMAT_JSON
} SourceFormat;

// Which member of a result holds what, for a site that answers with data rather than a feed. A feed
// has settled names for these; JSON does not, so each site says.
typedef struct {
   char list[SOURCE_FIELD_MAX];    // the member holding the array, empty when the answer is one
   char title[SOURCE_FIELD_MAX];
   char hash[SOURCE_FIELD_MAX];
   char size[SOURCE_FIELD_MAX];
   char seeds[SOURCE_FIELD_MAX];
   char peers[SOURCE_FIELD_MAX];
   char category[SOURCE_FIELD_MAX];   // the member holding the number the site files a torrent under
} SourceFields;

typedef struct {
   char         name[SOURCE_NAME_MAX];
   char         searchUrl[SOURCE_URL_MAX];
   char         torrentUrl[SOURCE_URL_MAX];   // empty when the results link straight to the file
   char         fileName[SOURCE_NAME_MAX];    // what a screen moves when it turns the source off
   SourceFormat format;
   SourceFields fields;

   // for a site that gives a hash and no file to fetch: a magnet is built from the hash and these
   char trackers[SOURCE_TRACKER_MAX][SOURCE_URL_MAX];
   int  trackerCount;

   // the run of category numbers the site keeps pornography in, so an app can leave it out. both 0
   // when the site says nothing, which is the only reason to fall back to reading titles.
   int  adultFrom;
   int  adultTo;

   int  enabled;
} TorrentSource;

// one source file an app ships, written into the folder the first time the app runs
typedef struct {
   const char *fileName;
   const char *text;
} SourceFile;

// Read every source in the folder and in its "disabled" folder, creating both and writing the files
// an app ships when they are not there yet. Returns how many were read, the disabled ones included.
// Sources past capacity are ignored.
int loadTorrentSources(const char *directory, TorrentSource *sources, int capacity, const SourceFile *shipped,
                       int shippedCount);

// Turn one on or off by moving its file. Returns 0, or -1 when the file could not be moved.
int setSourceEnabled(const char *directory, TorrentSource *source, int enabled);

// Put the query into the source's search address, escaping anything a URL cannot carry as it is.
// Returns 0, or -1 when nothing was typed or the result would not fit.
int buildSearchUrl(const TorrentSource *source, const char *query, char *out, int capacity);

// Where to fetch one torrent's file: the source's address with its hash filled in, or the address
// the results gave when the source has none or infoHash is null. Returns 0, or -1 when neither is
// usable.
int buildTorrentUrl(const TorrentSource *source, const uint8_t *infoHash, const char *resultLink, char *out,
                    int capacity);
