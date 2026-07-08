// youtube extractor - InnerTube player request + parse.
//
// POSTs the player endpoint as the ANDROID_VR (Oculus) client, which returns
// direct (unciphered) urls and - unlike plain ANDROID/WEB - is not behind the
// GVS PoToken wall, so its adaptive (fragmented) urls above 360p don't 403.
// Those streams are fragmented mp4 (moof); the demuxer handles them.
//
// Fragile by nature: Google keeps closing these token-exempt clients (VR must be
// pinned to 1.65.10 - newer builds hand back SABR-only streams). The durable fix
// is a multi-client fallback chain (VR -> ANDROID itag 18 -> ...) layered here.

#include "extractor.h"
#include "http-fetch.h"
#include "string-utilities.h"   // strCopy
#include "dbg.h"                // logInfo (resolve diagnostics)
#include "vfs.h"                // readFile/writeFile/deleteFile (persist the visitor session)

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <cell/http.h>

#define PLAYER_URL "https://www.youtube.com/youtubei/v1/player?prettyPrint=false"
#define SEARCH_URL "https://www.youtube.com/youtubei/v1/search?prettyPrint=false"
#define BROWSE_URL "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false"
#define RESP_CAP   (2 * 1024 * 1024)   // player JSON can pass 1 MB; a truncated response loses formats (and their urls)

// two clients, each for what it does best:
//  - ANDROID_VR (playback), see below. WEB/plain ANDROID are PoToken-gated for
//    adaptive (their googlevideo urls 403 above 360p - only progressive itag 18
//    is token-exempt); TVHTML5 hit the bot wall; iOS gave only fragmented adaptive.
//  - ANDROID_VR (Oculus) is currently exempt from the GVS PO-token wall and
//    returns direct (un-ciphered) adaptive urls, so it plays above 360p. Pin the
//    version to 1.65.10 - newer VR builds hand back SABR-only streams we can't
//    read. Erratic on some videos, so ANDROID itag 18 stays the 360p fallback.
//  - WEB (search): search isn't PoToken-gated, and WEB reliably returns the
//    videoRenderer result structure we parse.
#define VR_VERSION  "1.65.10"
#define WEB_VERSION "2.20250701.00.00"

// visitor session endpoint: mints an anonymous visitorData that clears the
// logged-out LOGIN_REQUIRED ("confirm you're not a bot") wall (see extract()).
#define VISITOR_URL "https://www.youtube.com/youtubei/v1/visitor_id?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false"

// the ANDROID_VR client object, without the wrapping braces so visitorData can
// be appended before the player request closes it.
#define VR_CLIENT \
   "\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"" VR_VERSION "\"," \
   "\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32," \
   "\"osName\":\"Android\",\"osVersion\":\"12L\",\"hl\":\"en\",\"gl\":\"US\"," \
   "\"timeZone\":\"UTC\",\"utcOffsetMinutes\":0"
#define WEB_CONTEXT \
   "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"" WEB_VERSION "\",\"hl\":\"en\",\"gl\":\"US\"}}"

static const char *VR_UA =
   "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
static const char *WEB_UA =
   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";

// player body: VR client + the minted visitorData, then the video id. two %s: visitorData, videoId.
static const char *PLAYER_BODY_FMT =
   "{\"context\":{\"client\":{" VR_CLIENT ",\"visitorData\":\"%s\"}},\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}";
static const char *VISITOR_BODY   = WEB_CONTEXT "}";   // WEB_CONTEXT leaves the root object open; close it
// search params combine the "Videos" filter (drops the Shorts shelf, channels and playlists) with a sort
// order. index by SortOrder. canonical values from youtube's own filter menu. two %s: escaped query, params.
static const char *SEARCH_SORT_PARAMS[SORT_COUNT] = {
   "EgIQAQ==",   // SORT_RELEVANCE (youtube's default)
   "CAMSAhAB",   // SORT_VIEWS (popularity / view count, highest first)
};
static const char *SEARCH_BODY_FMT = WEB_CONTEXT ",\"query\":\"%s\",\"params\":\"%s\"}";
// trending feed. YouTube retired the aggregated Trending page in 2025 (FEtrending now redirects to an empty
// home feed); NewPipe/FreeTube/Invidious replaced it with per-category channel-tab browses that still work
// logged-out. two %s: browseId, params. see TRENDING_CATEGORIES for the ids.
static const char *TRENDING_BODY_FMT = WEB_CONTEXT ",\"browseId\":\"%s\",\"params\":\"%s\"}";
static const char *CONTINUATION_BODY_FMT = WEB_CONTEXT ",\"continuation\":\"%s\"}";   // next page of any feed

static int hex4(const char *p)
{
   int value = 0;
   for (int i = 0; i < 4; i++) {
      char c = p[i], digit;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      else return -1;
      value = value * 16 + digit;
   }
   return value;
}

// copy the json string value of "key" found within [from,end), unescaping the
// usual escapes plus \uXXXX (ascii only - urls and titles are ascii; higher
// code points are dropped). returns 1 if the key was found.
static int jsonString(const char *from, const char *end, const char *key, char *out, int cap)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\":\"", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) { if (cap) out[0] = 0; return 0; }
   p += patLen;

   int i = 0;
   while (p < end && *p && *p != '"' && i < cap - 1) {
      if (*p != '\\') { out[i++] = *p++; continue; }
      if (++p >= end) break;                       // consume backslash
      char c = *p++;
      switch (c) {
         case 'n': out[i++] = '\n'; break;
         case 't': out[i++] = '\t'; break;
         case 'r': out[i++] = '\r'; break;
         case 'b': out[i++] = '\b'; break;
         case 'f': out[i++] = '\f'; break;
         case 'u': {
            if (p + 4 > end) { p = end; break; }
            int value = hex4(p);
            p += 4;
            if (value >= 0 && value <= 0x7F) out[i++] = (char)value;   // ascii only
            break;
         }
         default: out[i++] = c; break;              // \/ \\ \" and any other: keep literal
      }
   }
   out[i] = 0;
   return 1;
}

// read the (possibly quoted) integer value of "key" within [from,end). 1 if found.
static int jsonInt(const char *from, const char *end, const char *key, int *out)
{
   char pat[48];
   int patLen = snprintf(pat, sizeof pat, "\"%s\":", key);
   const char *p = from ? strstr(from, pat) : NULL;
   if (!p || p >= end) return 0;
   p += patLen;
   if (*p == '"') p++;      // some numeric fields are quoted (e.g. lengthSeconds)
   *out = atoi(p);
   return 1;
}

static int matches(const char *input)
{
   if (!input) return 0;
   if (strstr(input, "youtu")) return 1;     // youtube.com / youtu.be url
   return (int)strlen(input) == 11;          // bare 11-char video id
}

static int isIdChar(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// copy the 11-char youtube video id from a url (watch?v=, youtu.be/, /shorts/,
// /embed/, /live/) or a bare id into out. returns 1 only if a full 11-char id was found.
static int extractVideoId(const char *input, char *out, int cap)
{
   static const char *markers[] = { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" };
   const char *id = NULL;
   for (unsigned i = 0; i < sizeof markers / sizeof markers[0]; i++) {
      const char *hit = strstr(input, markers[i]);
      if (hit) { id = hit + strlen(markers[i]); break; }
   }
   if (!id) id = ((int)strlen(input) == 11) ? input : NULL;   // otherwise a bare id
   if (!id) return 0;

   int length = 0;
   while (length < 11 && length < cap - 1 && isIdChar(id[length])) { out[length] = id[length]; length++; }
   out[length] = 0;
   return length == 11;
}

// parse one formats array (region [arrayStart, regionEnd)) into out->formats.
// progressive entries are muxed (video+audio); adaptive entries are video-only
// or audio-only, classified from the mimeType.
static void parseFormatArray(const char *arrayStart, const char *regionEnd, int progressive, StreamInfo *out)
{
   const char *scan = arrayStart;
   while (out->formatCount < MAX_STREAM_FORMATS) {
      const char *object = strstr(scan, "\"itag\":");
      if (!object || object >= regionEnd) break;
      const char *next = strstr(object + 7, "\"itag\":");
      const char *objectEnd = (next && next < regionEnd) ? next : regionEnd;

      StreamFormat *format = &out->formats[out->formatCount];
      memset(format, 0, sizeof *format);
      jsonInt(object, objectEnd, "itag", &format->itag);
      jsonInt(object, objectEnd, "width", &format->width);
      jsonInt(object, objectEnd, "height", &format->height);
      jsonInt(object, objectEnd, "fps", &format->fps);
      format->needsCipher = !jsonString(object, objectEnd, "url", format->url, sizeof format->url);

      // mimeType is like: video/mp4; codecs="avc1.640028". only mark a stream playable when its codec is
      // one we can actually decode - H.264 (avc1/avc3) video, AAC (mp4a) audio. newer itags carry AV1/VP9
      // video or Opus audio in an mp4/webm container that our demuxer+decoder can't play (itag 787 = AV1),
      // and picking one of those over a real avc1 stream made the video fail to open.
      char mimeType[64] = "";
      jsonString(object, objectEnd, "mimeType", mimeType, sizeof mimeType);
      strCopy(format->container, sizeof format->container, strstr(mimeType, "webm") ? "webm" : "mp4");
      int isAvc = strstr(mimeType, "avc1") != NULL || strstr(mimeType, "avc3") != NULL;
      int isAac = strstr(mimeType, "mp4a") != NULL;
      if (progressive)                              format->hasVideo = format->hasAudio = 1;
      else if (strncmp(mimeType, "audio/", 6) == 0) format->hasAudio = isAac;
      else                                          format->hasVideo = isAvc;

      // keep only formats we can decode (H.264 video / AAC audio). dropping VP9/AV1/Opus frees array slots
      // so the AAC audio - which youtube lists AFTER all the video formats - isn't pushed past the cap.
      if (format->hasVideo || format->hasAudio) out->formatCount++;
      scan = objectEnd;
   }
}

// parse both stream arrays. iOS returns only adaptiveFormats (separate video and
// audio); WEB/TVHTML5 would also carry a progressive "formats" (muxed) array.
static void parseFormats(const char *resp, const char *end, StreamInfo *out)
{
   const char *streaming = strstr(resp, "\"streamingData\"");
   if (!streaming) return;
   const char *progressive = strstr(streaming, "\"formats\"");
   const char *adaptive    = strstr(streaming, "\"adaptiveFormats\"");

   if (progressive)
      parseFormatArray(progressive, (adaptive && adaptive > progressive) ? adaptive : end, 1, out);
   if (adaptive)
      parseFormatArray(adaptive, end, 0, out);
}

// The anonymous visitor session. Without it a logged-out player request gets LOGIN_REQUIRED. Minting
// a fresh one every launch is itself a bot signal that re-triggers the wall, so it persists to disk:
// we reuse a saved session and only mint (and save) when there's none, or a saved one goes stale.
#define VISITOR_CACHE_PATH "/dev_hdd0/tmp/yo-player/visitor.txt"

static char visitorData[192];
static int  visitorFromDisk;   // the current session was loaded from the cache (vs minted this launch)

static int loadVisitorData(void)
{
   return readFile(VISITOR_CACHE_PATH, visitorData, sizeof visitorData) > 0 && visitorData[0] != 0;
}

static void saveVisitorData(void)
{
   writeFile(VISITOR_CACHE_PATH, visitorData, strlen(visitorData));
}

// a saved session that hit LOGIN_REQUIRED is stale: forget it so the next play mints a fresh one once.
static void dropVisitorData(void)
{
   deleteFile(VISITOR_CACHE_PATH);
   visitorData[0] = 0;
   visitorFromDisk = 0;
}

static int ensureVisitorData(void)
{
   if (visitorData[0]) return 0;
   if (loadVisitorData()) { visitorFromDisk = 1; return 0; }   // reuse the persisted session

   CellHttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
   };
   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = httpFetch(CELL_HTTP_METHOD_POST, VISITOR_URL, headers, 4, VISITOR_BODY, (int)strlen(VISITOR_BODY), resp, RESP_CAP, &respLen, &status);
   int ok = (rc == 0 && status == 200 && jsonString(resp, resp + respLen, "visitorData", visitorData, sizeof visitorData));
   free(resp);

   if (!ok) { logError("[yt] visitor_id failed rc=%d status=%d\n", rc, status); return -1; }
   urlDecode(visitorData, visitorData, sizeof visitorData);   // endpoint returns it percent-encoded (%3D)
   visitorFromDisk = 0;
   logInfo("[yt] visitorData minted (%d chars)\n", (int)strlen(visitorData));
   return 0;
}

static int extract(const char *input, StreamInfo *out)
{
   memset(out, 0, sizeof *out);

   char videoId[16];
   if (!extractVideoId(input, videoId, sizeof videoId)) return -1;
   if (ensureVisitorData() != 0) return -1;

   char body[1024];
   int bodyLen = snprintf(body, sizeof body, PLAYER_BODY_FMT, visitorData, videoId);

   CellHttpHeader headers[] = {
      { "Content-Type", "application/json" },
      { "User-Agent", VR_UA },
      { "X-YouTube-Client-Name", "28" },
      { "X-YouTube-Client-Version", VR_VERSION },
      { "X-Goog-Visitor-Id", visitorData },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };

   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = httpFetch(CELL_HTTP_METHOD_POST, PLAYER_URL, headers, 7, body, bodyLen, resp, RESP_CAP, &respLen, &status);
   if (rc < 0)        { logError("[yt] player POST failed rc=%d\n", rc); free(resp); return rc; }
   if (status != 200) { logError("[yt] player POST status=%d respLen=%d\n", status, respLen); free(resp); return -status; }
   const char *end = resp + respLen;

   // metadata
   const char *details = strstr(resp, "\"videoDetails\"");
   jsonString(details ? details : resp, end, "title", out->title, sizeof out->title);
   char lenText[16];
   if (jsonString(details ? details : resp, end, "lengthSeconds", lenText, sizeof lenText))
      out->durationSeconds = atoi(lenText);

   parseFormats(resp, end, out);

   if (out->formatCount == 0) {
      char statusText[32] = "";
      const char *play = strstr(resp, "\"playabilityStatus\"");
      jsonString(play ? play : resp, end, "status", statusText, sizeof statusText);
      logError("[yt] no formats, respLen=%d playability=%s\n", respLen, statusText);
      if (visitorFromDisk && strcmp(statusText, "LOGIN_REQUIRED") == 0) dropVisitorData();   // stale session
   } else {
      logInfo("[yt] resolved %d formats\n", out->formatCount);
      if (!visitorFromDisk) { saveVisitorData(); visitorFromDisk = 1; }   // persist a session that works
   }

   free(resp);
   return 0;
}

// copy src into out as a json string body value, escaping " and \ (and dropping
// control chars). the query comes from the on-screen keyboard.
static void jsonEscape(const char *src, char *out, int cap)
{
   int i = 0;
   for (; *src && i < cap - 2; src++) {
      unsigned char c = (unsigned char)*src;
      if (c == '"' || c == '\\') { out[i++] = '\\'; out[i++] = (char)c; }
      else if (c >= 0x20)        { out[i++] = (char)c; }
   }
   out[i] = 0;
}

// pull a nested string from a videoRenderer sub-object: find `container` (e.g. "\"ownerText\""), then read
// `key` (e.g. "text" for a runs array, "simpleText" for a plain value) within the block. Missing = left empty.
static void readField(const char *block, const char *end, const char *container, const char *key, char *out, int cap)
{
   const char *pos = strstr(block, container);
   if (pos && pos < end) jsonString(pos, end, key, out, cap);
}

// the next feed item at or after `from`. search uses "videoRenderer", trending shelves "gridVideoRenderer",
// and the music channel the newer "lockupViewModel". returns the nearest of the three (NULL if none).
static const char *nextItemBlock(const char *from)
{
   static const char *keys[] = { "\"videoRenderer\"", "\"gridVideoRenderer\"", "\"lockupViewModel\"" };
   const char *best = NULL;
   for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
      const char *hit = strstr(from, keys[i]);
      if (hit && (!best || hit < best)) best = hit;
   }
   return best;
}

static int isDuplicate(const SearchResults *out, const char *videoId)
{
   for (int i = 0; i < out->count; i++) if (strcmp(out->items[i].videoId, videoId) == 0) return 1;
   return 0;
}

// copy the first "content":"..." value within [from,end) whose text ends with `suffix` (e.g. " views",
// " ago"). lockupViewModel metadata rows are plain content strings, not keyed by field.
static void readContentEndingWith(const char *from, const char *end, const char *suffix, char *out, int cap)
{
   int suffixLen = (int)strlen(suffix);
   const char *p = from;
   while ((p = strstr(p, "\"content\":\"")) != NULL && p + 11 <= end) {
      p += 11;                                       // past "content":"
      const char *close = memchr(p, '"', end - p);
      if (!close) break;
      int length = (int)(close - p);
      if (length >= suffixLen && length < cap && strncmp(close - suffixLen, suffix, suffixLen) == 0) {
         memcpy(out, p, length); out[length] = 0; return;
      }
      p = close + 1;
   }
}

// the owning channel's UC id: the first "browseId":"UC..." in the item block (the video owner for a
// videoRenderer, the author for a lockup). left empty if absent.
static void readChannelId(const char *block, const char *end, char *out, int cap)
{
   const char *p = strstr(block, "\"browseId\":\"UC");
   if (p && p < end) jsonString(p, end, "browseId", out, cap);
   else if (cap) out[0] = 0;
}

// fill one result from a videoRenderer (search) or gridVideoRenderer (trending) block. 1 if it has a title.
static int parseRendererItem(const char *block, const char *end, SearchResult *item)
{
   if (!jsonString(block, end, "videoId", item->videoId, sizeof item->videoId) || !item->videoId[0]) return 0;
   readChannelId(block, end, item->channelId, sizeof item->channelId);
   readField(block, end, "\"title\"", "text", item->title, sizeof item->title);
   if (!item->title[0])   // gridVideoRenderer (trending) uses a plain simpleText title
      readField(block, end, "\"title\"", "simpleText", item->title, sizeof item->title);
   readField(block, end, "\"lengthText\"",        "simpleText", item->duration,  sizeof item->duration);
   readField(block, end, "\"ownerText\"",         "text",       item->author,    sizeof item->author);
   if (!item->author[0])   // trending items carry the channel in shortBylineText instead
      readField(block, end, "\"shortBylineText\"", "text", item->author, sizeof item->author);
   readField(block, end, "\"viewCountText\"",     "simpleText", item->views,     sizeof item->views);
   readField(block, end, "\"publishedTimeText\"", "simpleText", item->published, sizeof item->published);
   const char *livePos = strstr(block, "\"style\":\"LIVE\"");   // thumbnailOverlayTimeStatusRenderer
   item->isLive = (livePos && livePos < end);
   return item->title[0] != 0;
}

// fill one result from a lockupViewModel VIDEO block (youtube's newer feed-item format, used by the music
// channel). 1 if it's a playable video with a title; albums/playlists (other contentTypes) are skipped.
static int parseLockupItem(const char *block, const char *end, SearchResult *item)
{
   const char *videoType = strstr(block, "\"contentType\":\"LOCKUP_CONTENT_TYPE_VIDEO\"");
   if (!videoType || videoType >= end) return 0;
   if (!jsonString(block, end, "contentId", item->videoId, sizeof item->videoId) || !item->videoId[0]) return 0;

   readChannelId(block, end, item->channelId, sizeof item->channelId);
   readField(block, end, "\"lockupMetadataViewModel\"", "content", item->title, sizeof item->title);
   readField(block, end, "\"metadataParts\"",           "content", item->author, sizeof item->author);
   // on a channel's own page the first metadata part is the view count, not the channel name - drop it so
   // the caller can substitute the real channel name (the channel is implied on that page).
   if (strstr(item->author, " views") || strstr(item->author, " watching")) item->author[0] = 0;
   readField(block, end, "\"thumbnailBadgeViewModel\"", "text",    item->duration, sizeof item->duration);
   readContentEndingWith(block, end, " views", item->views, sizeof item->views);
   readContentEndingWith(block, end, " ago",   item->published, sizeof item->published);
   item->isLive = 0;
   return item->title[0] != 0;
}

// scan a WEB innertube response for feed items (search, trending shelves and the music channel each use a
// different renderer, dispatched per block) and fill out->items. dedupes by videoId - youtube repeats a
// video across shelves.
static void parseVideoResults(const char *resp, int respLen, SearchResults *out)
{
   const char *end = resp + respLen;
   const char *scan = resp;
   while (out->count < MAX_SEARCH_RESULTS) {
      const char *block = nextItemBlock(scan);
      if (!block) break;
      const char *next = nextItemBlock(block + 1);
      const char *blockEnd = (next && next < end) ? next : end;

      SearchResult *item = &out->items[out->count];
      memset(item, 0, sizeof *item);
      int ok = (strncmp(block + 1, "lockupViewModel", 15) == 0)
             ? parseLockupItem(block, blockEnd, item)
             : parseRendererItem(block, blockEnd, item);
      // drop members-only videos: they resolve to UNPLAYABLE for an anonymous session (no stream, no author)
      const char *membersBadge = strstr(block, "Members only");
      if (membersBadge && membersBadge < blockEnd) ok = 0;
      if (ok && !isDuplicate(out, item->videoId)) out->count++;
      scan = blockEnd;
   }

   // the "load more" token for the next page. anchor on the OBJECT ("...":{) not the bare name - youtube's
   // response carries a schema list of all renderer names ("...","...") and the bare string matches that
   // first, yielding a garbage token. the real continuationItemRenderer holds continuationCommand > token.
   const char *more = strstr(resp, "\"continuationItemRenderer\":{");
   if (more && more < end) jsonString(more, end, "token", out->continuation, sizeof out->continuation);

   // channel feeds carry the channel's own title in metadata; used to label videos on a channel page that
   // omit their author (the channel is implied). absent in search/trending responses (left empty). anchor on
   // the OBJECT ("...":{) - the bare name appears first in youtube's schema list, and the title after it is a
   // tab ("Home"), not the channel name.
   const char *channelMeta = strstr(resp, "\"channelMetadataRenderer\":{");
   if (channelMeta && channelMeta < end) jsonString(channelMeta, end, "title", out->channelName, sizeof out->channelName);

   // channel Videos feeds carry Latest/Popular/Oldest sort chips; each chipViewModel holds a continuation
   // token the screen re-browses with to re-sort. absent in search/trending responses (left empty).
   static const char *SORT_CHIP_LABELS[3] = { "Latest", "Popular", "Oldest" };
   for (int chip = 0; chip < 3; chip++) {
      char pat[48];
      snprintf(pat, sizeof pat, "\"chipViewModel\":{\"text\":\"%s\"", SORT_CHIP_LABELS[chip]);
      const char *found = strstr(resp, pat);
      if (found && found < end) jsonString(found, end, "token", out->sortTokens[chip], MAX_SORT_TOKEN);
   }
}

// POST a WEB innertube endpoint with a prebuilt json body, then parse videoRenderers from the response.
static int postAndParse(const char *url, const char *body, int bodyLen, SearchResults *out)
{
   memset(out, 0, sizeof *out);
   char *resp = malloc(RESP_CAP);
   if (!resp) return -1;

   CellHttpHeader headers[] = {   // WEB_UA is a pointer, so these can't be a file-scope const array
      { "Content-Type", "application/json" },
      { "User-Agent", WEB_UA },
      { "X-YouTube-Client-Name", "1" },
      { "X-YouTube-Client-Version", WEB_VERSION },
      { "Origin", "https://www.youtube.com" },
      { "Accept-Encoding", "identity" },
   };
   int headerCount = sizeof headers / sizeof headers[0];

   int respLen = 0, status = 0;
   int rc = httpFetch(CELL_HTTP_METHOD_POST, url, headers, headerCount, body, bodyLen, resp, RESP_CAP, &respLen, &status);
   if (rc < 0)        { free(resp); return rc; }
   if (status != 200) { free(resp); return -status; }

   parseVideoResults(resp, respLen, out);
   free(resp);
   return 0;
}

static int search(const char *query, SortOrder sort, const char *continuation, SearchResults *out)
{
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (continuation) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, continuation);
   else {
      char escaped[256];
      jsonEscape(query, escaped, sizeof escaped);
      if (sort < 0 || sort >= SORT_COUNT) sort = SORT_RELEVANCE;
      bodyLen = snprintf(body, sizeof body, SEARCH_BODY_FMT, escaped, SEARCH_SORT_PARAMS[sort]);
   }
   return postAndParse(SEARCH_URL, body, bodyLen, out);
}

// the surviving per-category channel-tab feeds, each a real feed logged-out. order matches TrendingCategory.
static const struct { const char *browseId, *params; } TRENDING_FEEDS[TREND_COUNT] = {
   { "UCOpNcN46UbXVtpKMrmU4Abg", "Egh0cmVuZGluZw%3D%3D" },              // TREND_GAMING
// { "UCq-Fj5jknLsUf-MWSy4_brA", "Egh0cmVuZGluZw%3D%3D" },              // TREND_MUSIC (auto-generated Music channel) - disabled
   { "UC4R8DWoMoI7CAwX8_LjQHig", "EgdsaXZldGFikgEDCKEK" },              // TREND_LIVE
   { "UCEgdi0XIXXZ-qJOFPf4JSKw", "EglzcG9ydHN0YWK4AQCSAwDyBgQKAjIA" },  // TREND_SPORTS
   { "FEpodcasts_destination",   "qgcCCAM%3D" },                        // TREND_PODCASTS
};

static int fetchTrending(TrendingCategory category, const char *continuation, SearchResults *out)
{
   if (category < 0 || category >= TREND_COUNT) return -1;
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (continuation) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, continuation);
   else bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, TRENDING_FEEDS[category].browseId, TRENDING_FEEDS[category].params);
   return postAndParse(BROWSE_URL, body, bodyLen, out);
}

// a channel's Videos tab (sorted Latest by default). token NULL = first page via the videos-tab params;
// else a sort-chip or next-page continuation token. the response fills out->sortTokens for the sort chips.
#define CHANNEL_VIDEOS_PARAMS "EgZ2aWRlb3PyBgQKAjoA"
static int getChannel(const char *channelId, const char *token, SearchResults *out)
{
   char body[MAX_CONTINUATION + 512];
   int bodyLen;
   if (token) bodyLen = snprintf(body, sizeof body, CONTINUATION_BODY_FMT, token);
   else       bodyLen = snprintf(body, sizeof body, TRENDING_BODY_FMT, channelId, CHANNEL_VIDEOS_PARAMS);
   return postAndParse(BROWSE_URL, body, bodyLen, out);
}

const Extractor youtubeExtractor = { "youtube", matches, extract, search, fetchTrending, getChannel };
