#pragma once

// extractor - source-agnostic media extraction interface (yt-dlp / NewPipe
// style). Each site (youtube, and later others) implements one Extractor. The
// player never knows which site a stream came from; it just gets a StreamInfo.

#define MAX_STREAM_URL     2048
#define MAX_STREAM_FORMATS 24

// one selectable stream. url may be empty if the source needs deobfuscation we
// haven't done yet (e.g. youtube's cipher) -- formatCount still reflects what
// the source offered.
typedef struct {
   int  itag;              // source-specific format id (0 if n/a)
   int  width, height;
   int  fps;               // frames per second (0 if the source doesn't say)
   int  hasVideo, hasAudio;
   int  needsCipher;       // 1 = url is signature-ciphered, not yet playable
   char container[8];      // "mp4"
   char url[MAX_STREAM_URL];
   int  isLiveSegmented;   // url is a DASH segment base: fetch "<url>sq/<n>/..." instead of opening it directly
   long liveStartSq;       // earliest segment number available (DVR window start)
   long liveEdgeSq;        // most recent segment number available (live edge)
} StreamFormat;

typedef struct {
   char title[256];
   char description[2048];   // video description, truncated; newlines are real '\n'
   int  durationSeconds;
   int  isLive;            // currently broadcasting: streams need live segment-sequence fetching, not a static moov
   int  formatCount;
   StreamFormat formats[MAX_STREAM_FORMATS];
} StreamInfo;

// accumulated across pages (infinite scroll); one page is ~20. only the ~30 on-screen tiles hold a
// thumbnail texture + rasterised labels at once (windowed), so this bounds metadata + kept jpegs, not VRAM.
#define MAX_SEARCH_RESULTS 500

// one search hit. videoId is what playVideo() / extract() take; channelId enters that video's channel feed.
typedef struct {
   char videoId[16];
   char channelId[32];     // owning channel's UC id (for "open channel"); empty if not parsed
   char title[160];
   char duration[12];      // "3:15" (empty if unknown / live)
   char author[48];        // channel name
   char views[24];         // "6,100 views"
   char published[24];     // "1 hour ago"
   int  isLive;            // 1 = live now (no duration; shown as a red LIVE badge)
} SearchResult;

#define MAX_CONTINUATION 4096   // youtube "load more" tokens are long base64 blobs
#define MAX_SORT_TOKEN   512    // a channel sort chip's continuation token

typedef struct {
   int          count;
   char         continuation[MAX_CONTINUATION];   // token for the next page (empty = no more)
   char         channelName[48];                  // viewed channel's title (channel feeds only; else empty)
   // channel Videos feeds carry three sort chips (Latest/Popular/Oldest); each is a continuation token the
   // screen re-browses with. empty outside channel mode. index matches ChannelSort.
   char         sortTokens[3][MAX_SORT_TOKEN];
   SearchResult items[MAX_SEARCH_RESULTS];
} SearchResults;

// trending categories (YouTube retired the single aggregated feed; these are the surviving per-category
// channel-tab feeds). the UI cycles them; keep TrendingNames (home.c) and TRENDING_FEEDS (youtube.c) in sync.
typedef enum {
   TREND_GAMING,
   TREND_SPORTS,
   TREND_PODCASTS,
   TREND_COUNT
} TrendingCategory;

// search result ordering. YouTube's newer WEB API only offers these two sorts (its "Prioritize" menu
// dropped date-sort); the UI cycles them with SELECT. keep SortNames in the search screen in sync.
typedef enum {
   SORT_RELEVANCE,
   SORT_VIEWS,
   SORT_COUNT
} SortOrder;

// a channel Videos feed's sort chips. index matches SearchResults.sortTokens. keep ChannelSortNames in
// the search screen in sync with this order.
typedef enum {
   CHANNEL_SORT_LATEST,
   CHANNEL_SORT_POPULAR,
   CHANNEL_SORT_OLDEST,
   CHANNEL_SORT_COUNT
} ChannelSort;

typedef struct Extractor {
   const char *name;                                    // "youtube"
   int (*matches)(const char *input);                   // claim this url/id?
   int (*extract)(const char *input, StreamInfo *out);  // 0 ok, negative on error
   // continuation is NULL for the first page, or a prior result's token to fetch the next page.
   int (*search)(const char *query, SortOrder sort, const char *continuation, SearchResults *out);
   int (*getTrending)(TrendingCategory category, const char *continuation, SearchResults *out);
   // a channel's Videos feed. token NULL = first page (Latest); else a sort-chip or next-page token.
   int (*getChannel)(const char *channelId, const char *token, SearchResults *out);
} Extractor;

// picks the first registered extractor that claims input, or NULL.
const Extractor *findExtractor(const char *input);

// text search. continuation NULL = first page, else a prior result's token. 0 ok, negative on error.
int searchVideos(const char *query, SortOrder sort, const char *continuation, SearchResults *out);

// a trending category feed (the launch/home screen). continuation as above. 0 ok, negative on error.
int getTrending(TrendingCategory category, const char *continuation, SearchResults *out);

// a channel's Videos feed. token NULL = first page; else a sort-chip token (SearchResults.sortTokens) or a
// next-page continuation. the first page also fills out->sortTokens. 0 ok, negative on error.
int getChannelVideos(const char *channelId, const char *token, SearchResults *out);
