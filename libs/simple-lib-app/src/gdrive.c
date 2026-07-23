//
// gdrive.c - Google Drive VFS backend (see gdrive.h).
//
// It reads the OAuth client credentials from the app's settings.txt (seeding empty keys on
// first run) and publishes the "/Google Drive" mount only when those keys are filled in - an
// empty settings file means the user isn't using Drive, so no folder appears at the root. A
// configured mount still costs nothing until it is opened: that is when the sign-in happens.
// Browsing, downloads and uploads are all live - the mount behaves like any other volume.
//
// The mount's native prefix is "google:", so every op receives a path shaped like
// "google:/Folder/file"; getDrivePath() strips through the ':' to the in-Drive path.
//

#include "gdrive.h"

#include "vfs.h"
#include "http.h"
#include "settings-file.h"
#include "string-utilities.h"
#include "gdrive-crypto.h"
#include "thread.h"            // one lock: ops run from the UI thread and copy/paste task threads
#include "dbg.h"
#include <stdlib.h>            // malloc/free (app-side backend; not linked by plugins)
#include <sys/sys_time.h>     // sys_time_get_system_time (listing-cache TTL)

#define GDRIVE_SEGMENT   "Google Drive"   // root-visible folder name (also the /path component)
#define GDRIVE_NATIVE    "google:"        // prefix the ops get back ("google:/In/Drive/path")

#define CLIENT_ID_CAP      256
#define CLIENT_SECRET_CAP  128
#define TOKEN_CAP          512
#define SETTINGS_TEXT_CAP  4096
#define HTTP_RESPONSE_CAP  4096
#define BUNDLE_CAP         (CLIENT_ID_CAP + CLIENT_SECRET_CAP + TOKEN_CAP)
#define ENCRYPTED_HEX_CAP  (BUNDLE_CAP * 2 + 64)

// Google's OAuth token endpoint. The refresh token is obtained once in a browser on a PC (full Drive
// scope, which the on-console device flow is not allowed to grant); here we only ever refresh it.
#define TOKEN_URL        "https://oauth2.googleapis.com/token"

// configuration + auth state, loaded once at init. after the first successful connect these live in the
// console-bound "google_auth_enc" blob on disk, but freshly pasted plaintext keys always win over it.
static char clientId[CLIENT_ID_CAP];
static char clientSecret[CLIENT_SECRET_CAP];
static char refreshToken[TOKEN_CAP];
static char accessToken[TOKEN_CAP];       // short-lived; RAM only, never written to disk
static char authHeader[TOKEN_CAP + 8];    // "Bearer <accessToken>", rebuilt on each refresh
static char settingsPath[MAX_PATH_LEN];   // where the credential bundle is persisted
static int  credentialsEncrypted;         // 1 when loaded from (or saved to) the console-bound blob
static int  mounted;

static int isGdriveConfigured(void) { return clientId[0] != '\0'; }

// copies the value for key out of a loaded settings buffer, stopping at end of line or
// whitespace (so an inline "# comment" after the value is ignored). empties out if absent.
static void copySettingValue(const char *text, const char *key, char *out, int cap)
{
   out[0] = '\0';
   const char *value = findSettingValue(text, key);
   if (!value) return;
   int n = 0;
   while (value[n] && value[n] != '\n' && value[n] != '\r' && value[n] != ' ' && value[n] != '\t' && n < cap - 1) {
      out[n] = value[n];
      n++;
   }
   out[n] = '\0';
}

// "google:/Folder/file" -> "/Folder/file"; the in-Drive path always starts with '/'.
static const char *getDrivePath(const char *native)
{
   const char *colon = native;
   while (*colon && *colon != ':') colon++;
   return (*colon == ':') ? colon + 1 : native;
}

// section: JSON helpers

// extracts the string value of "key":"..." from a JSON body into out. 1 if found, 0 if not.
// minimal unescape (tokens/urls need no \uXXXX); operates on [json, json+jsonLength).
static int getJsonString(const char *json, int jsonLength, const char *key, char *out, int cap)
{
   char needle[80];
   int n = 0;
   needle[n++] = '"';
   for (const char *k = key; *k && n < (int)sizeof needle - 2; k++) needle[n++] = *k;
   needle[n++] = '"';

   int index = findBytes(json, jsonLength, needle, n);
   if (index < 0) { out[0] = '\0'; return 0; }

   const char *p = json + index + n, *end = json + jsonLength;
   while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) p++;
   if (p >= end || *p != '"') { out[0] = '\0'; return 0; }
   p++;

   int o = 0;
   while (p < end && *p != '"' && o < cap - 1) {
      if (*p == '\\' && p + 1 < end) {
         p++;
         out[o++] = (*p == 'n') ? '\n' : (*p == 't') ? '\t' : *p;
      } else {
         out[o++] = *p;
      }
      p++;
   }
   out[o] = '\0';
   return 1;
}

// appends "text" as a quoted JSON string (escaping what a filename can legally contain).
static void appendJsonString(char *out, int cap, int *off, const char *text)
{
   int o = *off;
   if (o < cap - 1) out[o++] = '"';
   for (const char *p = text; *p && o < cap - 2; p++) {
      if (*p == '"' || *p == '\\') { out[o++] = '\\'; if (o < cap - 1) out[o++] = *p; }
      else if ((unsigned char)*p >= 0x20) out[o++] = *p;   // control characters can't appear in a filename
   }
   if (o < cap - 1) out[o++] = '"';
   out[o] = '\0';
   *off = o;
}

// section: credential persistence (console-bound)

// splits the decrypted bundle (three newline-separated fields) back into the credential state.
static void parseBundle(const char *bundle)
{
   char *fields[3] = { clientId, clientSecret, refreshToken };
   int   caps[3]   = { CLIENT_ID_CAP, CLIENT_SECRET_CAP, TOKEN_CAP };
   const char *p = bundle;
   for (int f = 0; f < 3; f++) {
      int o = 0;
      while (*p && *p != '\n' && o < caps[f] - 1) fields[f][o++] = *p++;
      fields[f][o] = '\0';
      if (*p == '\n') p++;
   }
}

// encrypts client id + secret + refresh token into one console-bound blob, then strips the plaintext
// keys - so after the first successful sign-in, settings.txt holds only ciphertext useless off-console.
static void persistEncryptedBundle(void)
{
   char bundle[BUNDLE_CAP];
   int n = 0;
   appendStr(bundle, sizeof bundle, &n, clientId);     appendStr(bundle, sizeof bundle, &n, "\n");
   appendStr(bundle, sizeof bundle, &n, clientSecret); appendStr(bundle, sizeof bundle, &n, "\n");
   appendStr(bundle, sizeof bundle, &n, refreshToken);
   bundle[n] = '\0';

   char encryptedHex[ENCRYPTED_HEX_CAP];
   if (gdriveEncryptSecret(bundle, encryptedHex, sizeof encryptedHex) != 0) {
      logError("[gdrive] could not encrypt credentials; leaving plaintext in settings\n");
      return;
   }
   if (upsertSettingValue(settingsPath, "google_auth_enc", encryptedHex) != 0) {
      logError("[gdrive] could not save credentials; leaving plaintext in settings\n");
      return;
   }
   deleteSettingKey(settingsPath, "google_client_id");
   deleteSettingKey(settingsPath, "google_client_secret");
   deleteSettingKey(settingsPath, "google_refresh_token");
   logInfo("[gdrive] saved console-bound credentials, stripped plaintext keys\n");
}

// loads credentials, preferring freshly pasted plaintext keys over the stored blob. that order is what
// makes re-authorising possible: when a refresh token expires the user pastes new keys and they win,
// and the next successful connect re-encrypts them. the blob is only used when no plaintext keys exist.
static void loadCredentials(const char *text)
{
   clientId[0] = clientSecret[0] = refreshToken[0] = accessToken[0] = '\0';
   credentialsEncrypted = 0;

   copySettingValue(text, "google_client_id",     clientId,     sizeof clientId);
   copySettingValue(text, "google_client_secret", clientSecret, sizeof clientSecret);
   copySettingValue(text, "google_refresh_token", refreshToken, sizeof refreshToken);
   if (clientId[0] && clientSecret[0] && refreshToken[0]) return;

   char encryptedHex[ENCRYPTED_HEX_CAP];
   copySettingValue(text, "google_auth_enc", encryptedHex, sizeof encryptedHex);
   if (!encryptedHex[0]) return;

   char bundle[BUNDLE_CAP];
   if (gdriveDecryptSecret(encryptedHex, bundle, sizeof bundle) != 0) {
      logError("[gdrive] stored credentials failed to decrypt (moved to another console?)\n");
      return;
   }
   parseBundle(bundle);
   credentialsEncrypted = 1;
}

// section: connect (mint an access token from the stored refresh token)

static int refreshGdriveAccessToken(void)
{
   if (!clientId[0] || !clientSecret[0] || !refreshToken[0]) return -1;

   char body[TOKEN_CAP + CLIENT_ID_CAP + CLIENT_SECRET_CAP + 64];
   int n = 0;
   appendStr(body, sizeof body, &n, "client_id=");       appendUrlEnc(body, sizeof body, &n, clientId);
   appendStr(body, sizeof body, &n, "&client_secret=");  appendUrlEnc(body, sizeof body, &n, clientSecret);
   appendStr(body, sizeof body, &n, "&refresh_token=");  appendUrlEnc(body, sizeof body, &n, refreshToken);
   appendStr(body, sizeof body, &n, "&grant_type=refresh_token");
   body[n] = '\0';

   HttpHeader formHeader = { "Content-Type", "application/x-www-form-urlencoded" };
   char responseBuffer[HTTP_RESPONSE_CAP];
   int responseLength = 0, status = 0;
   int rc = fetchHttp("POST", TOKEN_URL, &formHeader, 1, body, getStrLen(body), responseBuffer,
                      sizeof responseBuffer, &responseLength, &status);
   if (rc < 0)        { logError("[gdrive] token refresh transport error rc=%d\n", rc); return -1; }
   if (status != 200) {
      logError("[gdrive] token refresh failed status=%d body=%s\n", status, responseBuffer);
      return -1;
   }
   if (!getJsonString(responseBuffer, responseLength, "access_token", accessToken, sizeof accessToken)) {
      logError("[gdrive] refresh: no access_token\n");
      return -1;
   }

   int headerOffset = 0;   // cache the "Bearer <token>" header for the authorized Drive requests
   appendStr(authHeader, sizeof authHeader, &headerOffset, "Bearer ");
   appendStr(authHeader, sizeof authHeader, &headerOffset, accessToken);
   authHeader[headerOffset] = '\0';
   return 0;
}

static int connectGdrive(void)
{
   if (!isGdriveAuthorized()) { logWarn("[gdrive] connect: creds missing\n"); return -1; }
   if (accessToken[0]) return 0;                                          // already have a live token this session
   logInfo("[gdrive] connect: refreshing access token...\n");            // diagnostic: before the blocking request
   if (refreshGdriveAccessToken() != 0) return -1;                        // invalid/expired/revoked (logs its reason)

   // first successful connect with plaintext creds: lock them into the console-bound blob
   if (!credentialsEncrypted) { persistEncryptedBundle(); credentialsEncrypted = 1; }
   logInfo("[gdrive] connected (token encrypted=%d)\n", credentialsEncrypted);
   return 0;
}

// section: Drive listing cache + browsing
//
// Drive is id-based, not path-based, so the backend resolves a path to a file id by listing each
// ancestor folder and matching child names. One short-TTL cache of "folder path -> its children (with
// ids/sizes/dates)" backs everything: readdir snapshots it, stat and path->id resolution read it.

#define GDRIVE_ID_CAP       96
#define LISTING_CACHE_SLOTS 8
#define LISTING_TTL_US      30000000ULL      // re-list a folder at most once per 30s
#define QUOTA_TTL_US        60000000ULL      // re-ask Drive for the storage quota at most once a minute
#define LIST_PAGE_SIZE      "300"            // kept small so one page stays well under LIST_RESPONSE_CAP
#define LIST_RESPONSE_CAP   (256 * 1024)
#define MAX_LIST_PAGES      64               // a folder past this is truncated, but the loop always ends
#define MAX_OPEN_FILES      4
#define API_FILES           "https://www.googleapis.com/drive/v3/files"
#define API_UPLOAD          "https://www.googleapis.com/upload/drive/v3/files"
#define API_ABOUT           "https://www.googleapis.com/drive/v3/about?fields=storageQuota"
#define FOLDER_MIME         "application/vnd.google-apps.folder"
#define GOOGLE_NATIVE_MIME  "application/vnd.google-apps."   // Docs/Sheets/Slides/... - no downloadable bytes

// the tree walkers in vfs.c hold one open dir handle per level of nesting, so this pool must cover the
// deepest tree they will walk (MAX_TREE_DEPTH) - a smaller pool fails a copy partway down.
#define MAX_OPEN_DIRS       MAX_TREE_DEPTH

typedef struct {
   char     name[256];
   char     id[GDRIVE_ID_CAP];
   int      isDir;
   int      isGoogleNative;   // a Doc/Sheet/Slide: listable, but it has no bytes to download
   uint64_t size;
   uint64_t mtime;
} DriveChild;

typedef struct {
   char        path[MAX_PATH_LEN];   // the folder whose children these are ("/" = Drive root)
   DriveChild *children;
   int         count, capacity;
   uint64_t    fetchedAt;            // 0 = empty slot
} Listing;
static Listing listingCache[LISTING_CACHE_SLOTS];

typedef struct { DriveChild *children; int count, cursor; int used; } DirState;
static DirState dirPool[MAX_OPEN_DIRS];

// an open file downloads through a bounce buffer: fetchHttp always reserves one byte of its output for a
// NUL terminator, so downloading straight into the caller's buffer would always come up one byte short.
// the buffer grows to the caller's read size (fewer, bigger range requests) between these bounds.
// an upload streams through the same buffer in the other direction: Drive takes a file as a series of
// PUTs to an upload session, each carrying a multiple of 256 KB except the last one.
#define CHUNK_MIN_BYTES    (64 * 1024)
#define CHUNK_MAX_BYTES    (1024 * 1024)
#define UPLOAD_CHUNK_BYTES (1024 * 1024)
#define SESSION_URL_CAP    1024

typedef struct {
   char     id[GDRIVE_ID_CAP];
   uint64_t position, size;
   char    *chunk;         // chunkCapacity + 1 bytes (the extra byte is fetchHttp's terminator)
   uint64_t chunkStart;    // reading: file offset of chunk[0]
   int      chunkCapacity, chunkLength;
   int      writing;       // 1 while this handle is an upload (chunk holds bytes not yet sent)
   char     sessionUrl[SESSION_URL_CAP];
   char     parentPath[MAX_PATH_LEN];   // folder whose cached listing the finished upload updates
   char     fileName[256];
   uint64_t uploaded;      // bytes already accepted by Drive
   int      used;
} FileState;
static FileState filePool[MAX_OPEN_FILES];

// one lock guards the cache + handle pools + token: browsing runs on the UI thread and copy/paste on
// task threads. held for the whole op, including its network round trips - contention is low and Drive
// is serial anyway. created in initGdrive before the mount is published; if it can't be created, the
// mount is never published, so no op can run unlocked.
static sys_lwmutex_t gdriveLock;
static void lockGdrive(void)   { lock(&gdriveLock); }
static void unlockGdrive(void) { unlock(&gdriveLock); }

// section: small parsers

static uint64_t parseU64(const char *text)
{
   uint64_t value = 0;
   while (*text >= '0' && *text <= '9') value = value * 10 + (uint64_t)(*text++ - '0');
   return value;
}

static int parseDigits(const char *text, int digitCount)
{
   int value = 0;
   for (int i = 0; i < digitCount; i++) value = value * 10 + (text[i] - '0');
   return value;
}

// civil date -> days since 1970-01-01 (Howard Hinnant's algorithm)
static int64_t daysFromCivil(int year, int month, int day)
{
   year -= month <= 2;
   int64_t era = (year >= 0 ? year : year - 399) / 400;
   int yearOfEra = (int)(year - era * 400);
   int dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
   int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
   return era * 146097 + dayOfEra - 719468;
}

// "2024-01-15T10:30:00.000Z" (RFC 3339, always UTC from Drive) -> unix seconds
static uint64_t parseRfc3339(const char *text)
{
   if (getStrLen(text) < 19) return 0;
   int64_t days = daysFromCivil(parseDigits(text, 4), parseDigits(text + 5, 2), parseDigits(text + 8, 2));
   return (uint64_t)(days * 86400 + parseDigits(text + 11, 2) * 3600 + parseDigits(text + 14, 2) * 60
                     + parseDigits(text + 17, 2));
}

// splits "/Foo/bar" into parent "/Foo" + name "bar" (parent "/" for a top-level entry). nameOut is
// optional - callers that only need the parent pass NULL.
static void parentAndName(const char *path, char *parentOut, int parentCap, const char **nameOut)
{
   int slash = -1;
   for (int i = getStrLen(path) - 1; i >= 0; i--) if (path[i] == '/') { slash = i; break; }
   if (slash <= 0) { strCopy(parentOut, parentCap, "/"); if (nameOut) *nameOut = path + 1; return; }
   int n = slash < parentCap - 1 ? slash : parentCap - 1;
   memCopy(parentOut, path, n);
   parentOut[n] = '\0';
   if (nameOut) *nameOut = path + slash + 1;
}

// "https://.../files/<id><suffix>" - the shape every per-file call needs. returns the written length so
// a caller can keep appending query parameters.
static int buildDriveFileUrl(char *out, int cap, const char *base, const char *id, const char *suffix)
{
   int n = 0;
   appendStr(out, cap, &n, base);
   appendStr(out, cap, &n, "/");
   appendStr(out, cap, &n, id);
   if (suffix) appendStr(out, cap, &n, suffix);
   out[n] = '\0';
   return n;
}

// section: authorized Drive requests

// one request to Drive, signed with the bearer token. only method/url/responseBuffer/responseCapacity
// are always needed; the rest default to zero through a designated initializer.
typedef struct {
   const char *method;             // "GET" / "POST" / "PATCH" / "PUT" / "DELETE"
   const char *url;
   const char *range;              // download: "bytes=a-b"
   const char *contentRange;       // upload:   "bytes a-b/total"
   const char *contentType;        // body type, when there is a body
   const void *body;
   int         bodyLength;
   char       *responseBuffer;     // response body lands here, NUL-terminated
   int         responseCapacity;
   int         status;             // out: the HTTP code
   HttpHeaderCapture *capture;     // optional response header to copy out (upload session "Location")
} DriveRequest;

// sends the request, refreshing the access token once on a 401. returns the response body length, or -1.
static int sendDriveRequest(DriveRequest *request)
{
   if (!accessToken[0] && connectGdrive() != 0) return -1;
   for (int attempt = 0; attempt < 2; attempt++) {
      HttpHeader headers[4];
      int headerCount = 0;
      headers[headerCount++] = (HttpHeader){ "Authorization", authHeader };
      if (request->range)        headers[headerCount++] = (HttpHeader){ "Range", request->range };
      if (request->contentRange) headers[headerCount++] = (HttpHeader){ "Content-Range", request->contentRange };
      if (request->contentType)  headers[headerCount++] = (HttpHeader){ "Content-Type", request->contentType };

      int responseLength = 0;
      request->status = 0;
      uint64_t startedAt = sys_time_get_system_time();
      int rc = fetchHttpCapturing(request->method, request->url, headers, headerCount, request->body,
                                  request->bodyLength, request->responseBuffer, request->responseCapacity,
                                  &responseLength, &request->status, request->capture);
      logInfo("[gdrive] %s %dms status=%d sent=%d got=%d\n", request->method,
              (int)((sys_time_get_system_time() - startedAt) / 1000), request->status, request->bodyLength,
              responseLength);
      if (rc < 0) return rc;
      if (request->status == 401 && attempt == 0) {
         accessToken[0] = '\0';
         if (refreshGdriveAccessToken() != 0) return -1;
         continue;
      }
      return responseLength;
   }
   return -1;
}

// section: listing cache

static DriveChild *findChild(Listing *listing, const char *name)
{
   for (int i = 0; i < listing->count; i++) if (strEq(listing->children[i].name, name)) return &listing->children[i];
   return NULL;
}

// Drive allows two children of one folder to share a name, so a path can mean more than one file.
static int countChildrenNamed(Listing *listing, const char *name)
{
   int count = 0;
   for (int i = 0; i < listing->count; i++) if (strEq(listing->children[i].name, name)) count++;
   return count;
}

static void appendChild(Listing *listing, const DriveChild *child)
{
   if (listing->count >= listing->capacity) {
      int newCapacity = listing->capacity ? listing->capacity * 2 : 64;
      DriveChild *grown = realloc(listing->children, sizeof(DriveChild) * newCapacity);
      if (!grown) return;   // drop this child rather than crash; the listing is best-effort
      listing->children = grown;
      listing->capacity = newCapacity;
   }
   listing->children[listing->count++] = *child;
}

// parses the "files":[...] array of one files.list page into the listing, skipping braces inside strings.
static void parseFilesInto(const char *json, int jsonLength, Listing *listing)
{
   int index = findBytes(json, jsonLength, "\"files\"", 7);
   if (index < 0) return;
   const char *p = json + index + 7, *end = json + jsonLength;
   while (p < end && *p != '[') p++;
   if (p < end) p++;

   while (p < end) {
      // find one object and its extent
      while (p < end && *p != '{' && *p != ']') p++;
      if (p >= end || *p == ']') break;

      const char *objectStart = p;
      int depth = 0, inString = 0;
      while (p < end) {
         char c = *p;
         if (inString) { if (c == '\\') p++; else if (c == '"') inString = 0; }
         else if (c == '"') inString = 1;
         else if (c == '{') depth++;
         else if (c == '}') { depth--; if (depth == 0) { p++; break; } }
         p++;
      }
      int objectLength = (int)(p - objectStart);

      // turn it into a child
      DriveChild child;
      memSet(&child, 0, sizeof child);
      char mime[96], numbers[40];
      if (!getJsonString(objectStart, objectLength, "id", child.id, sizeof child.id) ||
          !getJsonString(objectStart, objectLength, "name", child.name, sizeof child.name))
         continue;

      getJsonString(objectStart, objectLength, "mimeType", mime, sizeof mime);
      child.isDir = strEq(mime, FOLDER_MIME);
      child.isGoogleNative = !child.isDir && startsWith(mime, GOOGLE_NATIVE_MIME);
      if (getJsonString(objectStart, objectLength, "size", numbers, sizeof numbers)) child.size = parseU64(numbers);
      if (getJsonString(objectStart, objectLength, "modifiedTime", numbers, sizeof numbers))
         child.mtime = parseRfc3339(numbers);
      appendChild(listing, &child);
   }
}

// fetches every page of folderId's children into listing (which is reset first). 0 / -1.
static int fetchListing(const char *folderId, Listing *listing)
{
   listing->count = 0;
   char *responseBuffer = malloc(LIST_RESPONSE_CAP);
   if (!responseBuffer) return -1;

   char pageToken[512], previousToken[512];
   pageToken[0] = previousToken[0] = '\0';
   int rc = 0;
   for (int page = 0; page < MAX_LIST_PAGES; page++) {
      char query[128];
      int q = 0;
      appendStr(query, sizeof query, &q, "'");
      appendStr(query, sizeof query, &q, folderId);
      appendStr(query, sizeof query, &q, "' in parents and trashed=false");
      query[q] = '\0';

      char url[768];
      int u = 0;
      appendStr(url, sizeof url, &u, API_FILES "?q=");
      appendUrlEnc(url, sizeof url, &u, query);
      appendStr(url, sizeof url, &u, "&fields=");
      appendUrlEnc(url, sizeof url, &u, "nextPageToken,files(id,name,mimeType,size,modifiedTime)");
      appendStr(url, sizeof url, &u, "&pageSize=" LIST_PAGE_SIZE);
      if (pageToken[0]) { appendStr(url, sizeof url, &u, "&pageToken="); appendUrlEnc(url, sizeof url, &u, pageToken); }
      url[u] = '\0';

      DriveRequest request = { .method = "GET", .url = url,
                               .responseBuffer = responseBuffer, .responseCapacity = LIST_RESPONSE_CAP };
      int length = sendDriveRequest(&request);
      if (length < 0 || request.status != 200) {
         logError("[gdrive] files.list status=%d body=%s\n", request.status, responseBuffer);
         rc = -1;
         break;
      }

      parseFilesInto(responseBuffer, length, listing);
      strCopy(previousToken, sizeof previousToken, pageToken);
      if (!getJsonString(responseBuffer, length, "nextPageToken", pageToken, sizeof pageToken)) break;

      // a token that never changes would page forever; treat it as the end of the folder
      if (strEq(pageToken, previousToken)) { logWarn("[gdrive] files.list repeated its page token\n"); break; }
   }

   free(responseBuffer);
   return rc;
}

static int resolveId(const char *path, char *idOut, int idCap);   // forward: ensureListing <-> resolveId recurse

// returns a fresh listing for folderPath (fetching if absent or past its TTL), or NULL on error.
static Listing *ensureListing(const char *folderPath)
{
   uint64_t now = sys_time_get_system_time();

   // reuse the slot already holding this path (refresh in place if stale); else the oldest slot
   Listing *slot = NULL;
   for (int i = 0; i < LISTING_CACHE_SLOTS; i++) {
      if (listingCache[i].fetchedAt && strEq(listingCache[i].path, folderPath)) {
         if (now - listingCache[i].fetchedAt < LISTING_TTL_US) return &listingCache[i];
         slot = &listingCache[i];   // stale: refetch into the same buffer
         break;
      }
   }
   if (!slot) {
      slot = &listingCache[0];
      for (int i = 1; i < LISTING_CACHE_SLOTS; i++)
         if (listingCache[i].fetchedAt < slot->fetchedAt) slot = &listingCache[i];
   }

   char folderId[GDRIVE_ID_CAP];
   if (resolveId(folderPath, folderId, sizeof folderId) != 0) return NULL;

   strCopy(slot->path, sizeof slot->path, folderPath);
   if (fetchListing(folderId, slot) != 0) { slot->fetchedAt = 0; return NULL; }
   slot->fetchedAt = now;
   return slot;
}

// resolves a drive path to its file id ("/" -> "root"). lists the parent to find the child. 0 / -1.
static int resolveId(const char *path, char *idOut, int idCap)
{
   if (path[0] == '/' && path[1] == '\0') { strCopy(idOut, idCap, "root"); return 0; }
   char parent[MAX_PATH_LEN];
   const char *name;
   parentAndName(path, parent, sizeof parent, &name);
   Listing *listing = ensureListing(parent);
   if (!listing) return -1;
   DriveChild *child = findChild(listing, name);
   if (!child) return -1;
   strCopy(idOut, idCap, child->id);
   return 0;
}

// as resolveId, but refuses when the name matches more than one child: reading the first of two
// same-named files is a guess we can live with, deleting or overwriting the wrong one is not.
static int resolveIdForChange(const char *path, char *idOut, int idCap)
{
   char parent[MAX_PATH_LEN];
   const char *name;
   parentAndName(path, parent, sizeof parent, &name);
   Listing *listing = ensureListing(parent);
   if (!listing) return -1;
   if (countChildrenNamed(listing, name) > 1) {
      logError("[gdrive] \"%s\" names more than one file in Drive; refusing to change either\n", name);
      return -1;
   }
   return resolveId(path, idOut, idCap);
}

// finds a drive path's own entry by listing its parent. returns a pointer into the (still-fresh) cache
// - copy what you need before the next ensureListing call. NULL if the parent or the child is missing.
static DriveChild *lookupChild(const char *path)
{
   char parent[MAX_PATH_LEN];
   const char *name;
   parentAndName(path, parent, sizeof parent, &name);
   Listing *listing = ensureListing(parent);
   return listing ? findChild(listing, name) : NULL;
}

// section: keeping the cache in step with changes we make
//
// After a change we patch the cached listing to match instead of dropping it: the screen is redrawn
// from that cache the moment an action finishes, so re-listing the folder would cost a second round
// trip to Google before anything appeared. Every change here is one we just made, so we know the
// result exactly.

// the folder's cached children, or NULL if that folder isn't cached (nothing to keep in step).
static Listing *findCachedListing(const char *folderPath)
{
   for (int i = 0; i < LISTING_CACHE_SLOTS; i++)
      if (listingCache[i].fetchedAt && strEq(listingCache[i].path, folderPath)) return &listingCache[i];
   return NULL;
}

static void removeCachedChild(const char *folderPath, const char *name)
{
   Listing *listing = findCachedListing(folderPath);
   DriveChild *child = listing ? findChild(listing, name) : NULL;
   if (!child) return;
   int index = (int)(child - listing->children);
   for (int i = index; i < listing->count - 1; i++) listing->children[i] = listing->children[i + 1];
   listing->count--;
}

// adds (or updates in place) one child of an already-cached folder.
static void putCachedChild(const char *folderPath, const DriveChild *child)
{
   Listing *listing = findCachedListing(folderPath);
   if (!listing) return;
   DriveChild *existing = findChild(listing, child->name);
   if (existing) *existing = *child;
   else          appendChild(listing, child);
}

// section: changing Drive (create / upload / delete / rename)

// builds {"name":"...","parents":["id"],"mimeType":"..."} - parentId and mimeType are optional.
static void buildFileMetadata(char *out, int cap, const char *name, const char *parentId, const char *mimeType)
{
   int n = 0;
   appendStr(out, cap, &n, "{\"name\":");
   appendJsonString(out, cap, &n, name);
   if (parentId) {
      appendStr(out, cap, &n, ",\"parents\":[");
      appendJsonString(out, cap, &n, parentId);
      appendStr(out, cap, &n, "]");
   }
   if (mimeType) {
      appendStr(out, cap, &n, ",\"mimeType\":");
      appendJsonString(out, cap, &n, mimeType);
   }
   appendStr(out, cap, &n, "}");
   out[n] = '\0';
}

// POST/PATCH against the files API; logs and fails on any non-2xx. returns the response body length
// (responseBuffer is NUL-terminated), or -1.
static int mutateDrive(const char *what, const char *method, const char *url, const char *jsonBody,
                       char *responseBuffer, int responseCapacity)
{
   DriveRequest request = { .method = method, .url = url,
                            .contentType = jsonBody ? "application/json" : NULL,
                            .body = jsonBody, .bodyLength = jsonBody ? getStrLen(jsonBody) : 0,
                            .responseBuffer = responseBuffer, .responseCapacity = responseCapacity };
   int length = sendDriveRequest(&request);
   if (length < 0 || request.status < 200 || request.status > 299) {
      logError("[gdrive] %s status=%d body=%s\n", what, request.status, length > 0 ? responseBuffer : "");
      return -1;
   }
   return length;
}

// the modifiedTime Drive stamped on a file it just created or changed (0 if it didn't say).
static uint64_t parseModifiedTime(const char *responseBuffer, int length)
{
   char text[40];
   return getJsonString(responseBuffer, length, "modifiedTime", text, sizeof text) ? parseRfc3339(text) : 0;
}

static int mkdirGdriveImpl(const char *native)
{
   const char *path = getDrivePath(native);
   char parent[MAX_PATH_LEN];
   const char *name;
   parentAndName(path, parent, sizeof parent, &name);

   // Drive is happy to hold two folders with the same name in one parent, so an unchecked create
   // would duplicate rather than no-op - and a merge (which calls mkdir on an existing folder) would
   // then resolve that name to whichever copy came first.
   DriveChild *existing = lookupChild(path);
   if (existing) return existing->isDir ? 0 : -1;

   char parentId[GDRIVE_ID_CAP];
   if (resolveId(parent, parentId, sizeof parentId) != 0) return -1;

   char body[512];
   buildFileMetadata(body, sizeof body, name, parentId, FOLDER_MIME);

   char responseBuffer[HTTP_RESPONSE_CAP];
   int length = mutateDrive("mkdir", "POST", API_FILES "?fields=id,modifiedTime", body,
                            responseBuffer, sizeof responseBuffer);
   if (length < 0) return -1;

   DriveChild created;
   memSet(&created, 0, sizeof created);
   if (getJsonString(responseBuffer, length, "id", created.id, sizeof created.id)) {
      strCopy(created.name, sizeof created.name, name);
      created.isDir = 1;
      created.mtime = parseModifiedTime(responseBuffer, length);
      putCachedChild(parent, &created);
   }
   return 0;
}

// removing moves the entry to Drive's trash rather than destroying it: this is the second half of every
// move, and every other Drive client trashes, so a mistaken delete stays recoverable for 30 days.
static int removeGdriveImpl(const char *native)
{
   const char *path = getDrivePath(native);
   char id[GDRIVE_ID_CAP];
   if (resolveIdForChange(path, id, sizeof id) != 0) return -1;

   char url[64 + GDRIVE_ID_CAP];
   buildDriveFileUrl(url, sizeof url, API_FILES, id, "?fields=id");

   char responseBuffer[HTTP_RESPONSE_CAP];
   if (mutateDrive("trash", "PATCH", url, "{\"trashed\":true}", responseBuffer, sizeof responseBuffer) < 0) return -1;

   char parent[MAX_PATH_LEN];
   const char *name;
   parentAndName(path, parent, sizeof parent, &name);
   removeCachedChild(parent, name);
   return 0;
}

// a rename and a move are the same call: the new name in the body, the folder change in the query.
static int renameGdriveImpl(const char *fromNative, const char *toNative)
{
   const char *from = getDrivePath(fromNative), *to = getDrivePath(toNative);
   char id[GDRIVE_ID_CAP];
   if (resolveIdForChange(from, id, sizeof id) != 0) return -1;

   char fromParent[MAX_PATH_LEN], toParent[MAX_PATH_LEN];
   const char *fromName, *toName;
   parentAndName(from, fromParent, sizeof fromParent, &fromName);
   parentAndName(to, toParent, sizeof toParent, &toName);

   // keep the entry's own details so the destination folder's cache can be patched after the move
   DriveChild moved;
   memSet(&moved, 0, sizeof moved);
   Listing *fromListing = findCachedListing(fromParent);
   DriveChild *entry = fromListing ? findChild(fromListing, fromName) : NULL;
   if (entry) moved = *entry;

   char url[256 + GDRIVE_ID_CAP];
   int u = buildDriveFileUrl(url, sizeof url, API_FILES, id, "?fields=id");
   if (!strEq(fromParent, toParent)) {
      char fromId[GDRIVE_ID_CAP], toId[GDRIVE_ID_CAP];
      if (resolveId(fromParent, fromId, sizeof fromId) != 0 || resolveId(toParent, toId, sizeof toId) != 0) return -1;
      appendStr(url, sizeof url, &u, "&addParents=");    appendUrlEnc(url, sizeof url, &u, toId);
      appendStr(url, sizeof url, &u, "&removeParents="); appendUrlEnc(url, sizeof url, &u, fromId);
      url[u] = '\0';
   }

   char body[512];
   buildFileMetadata(body, sizeof body, toName, NULL, NULL);
   char responseBuffer[HTTP_RESPONSE_CAP];
   if (mutateDrive("rename", "PATCH", url, body, responseBuffer, sizeof responseBuffer) < 0) return -1;

   removeCachedChild(fromParent, fromName);
   if (entry) {
      strCopy(moved.name, sizeof moved.name, toName);
      putCachedChild(toParent, &moved);
   }
   return 0;
}

// opens an upload session for path, replacing the file if it already exists. the session url Drive
// hands back in the Location header is where every chunk is PUT.
static int startUploadSession(FileState *state, const char *path)
{
   char parent[MAX_PATH_LEN];
   const char *name;
   parentAndName(path, parent, sizeof parent, &name);

   Listing *parentListing = ensureListing(parent);
   if (!parentListing) return -1;
   if (countChildrenNamed(parentListing, name) > 1) {
      logError("[gdrive] \"%s\" names more than one file in Drive; refusing to overwrite either\n", name);
      return -1;
   }

   DriveChild *existing = findChild(parentListing, name);
   char existingId[GDRIVE_ID_CAP];
   existingId[0] = '\0';
   if (existing && !existing->isDir) strCopy(existingId, sizeof existingId, existing->id);
   else if (existing) return -1;   // a folder already owns this name

   char parentId[GDRIVE_ID_CAP];
   if (!existingId[0] && resolveId(parent, parentId, sizeof parentId) != 0) return -1;

   char url[64 + GDRIVE_ID_CAP];
   int u = 0;
   appendStr(url, sizeof url, &u, API_UPLOAD);
   if (existingId[0]) { appendStr(url, sizeof url, &u, "/"); appendStr(url, sizeof url, &u, existingId); }
   appendStr(url, sizeof url, &u, "?uploadType=resumable");
   url[u] = '\0';

   char body[512];
   buildFileMetadata(body, sizeof body, name, existingId[0] ? NULL : parentId, NULL);

   char responseBuffer[HTTP_RESPONSE_CAP];
   HttpHeaderCapture location = { "location", state->sessionUrl, sizeof state->sessionUrl };
   DriveRequest request = { .method = existingId[0] ? "PATCH" : "POST", .url = url,
                            .contentType = "application/json", .body = body, .bodyLength = getStrLen(body),
                            .responseBuffer = responseBuffer, .responseCapacity = sizeof responseBuffer,
                            .capture = &location };
   int length = sendDriveRequest(&request);
   if (length < 0 || request.status != 200 || !state->sessionUrl[0]) {
      logError("[gdrive] upload session status=%d body=%s\n", request.status, length > 0 ? responseBuffer : "");
      return -1;
   }
   strCopy(state->parentPath, sizeof state->parentPath, parent);
   strCopy(state->fileName, sizeof state->fileName, name);
   return 0;
}

// PUTs the buffered bytes to the session. final closes the upload (and may carry no bytes at all).
static int uploadChunk(FileState *state, int final)
{
   // Content-Range: the byte span this PUT carries, then the total - "*" until we know it is the last
   char contentRange[80];
   int r = 0;
   appendStr(contentRange, sizeof contentRange, &r, "bytes ");
   if (state->chunkLength > 0) {
      r = appendUint64(contentRange, sizeof contentRange, r, state->uploaded);
      appendStr(contentRange, sizeof contentRange, &r, "-");
      r = appendUint64(contentRange, sizeof contentRange, r, state->uploaded + state->chunkLength - 1);
   } else {
      appendStr(contentRange, sizeof contentRange, &r, "*");
   }
   appendStr(contentRange, sizeof contentRange, &r, "/");
   if (final) r = appendUint64(contentRange, sizeof contentRange, r, state->uploaded + state->chunkLength);
   else       appendStr(contentRange, sizeof contentRange, &r, "*");
   contentRange[r] = '\0';

   char responseBuffer[HTTP_RESPONSE_CAP];
   DriveRequest request = { .method = "PUT", .url = state->sessionUrl, .contentRange = contentRange,
                            .contentType = "application/octet-stream",
                            .body = state->chunk, .bodyLength = state->chunkLength,
                            .responseBuffer = responseBuffer, .responseCapacity = sizeof responseBuffer };
   int length = sendDriveRequest(&request);

   // 308 "resume incomplete" is the success answer to every chunk but the last; the last answers 200/201
   int ok = final ? (request.status == 200 || request.status == 201) : (request.status == 308);
   if (length < 0 || !ok) {
      logError("[gdrive] upload chunk status=%d range=%s body=%s\n", request.status, contentRange,
               length > 0 ? responseBuffer : "");
      return -1;
   }
   state->uploaded += (uint64_t)state->chunkLength;
   state->chunkLength = 0;

   // the finished file's own record comes back with the last chunk, so the folder needs no re-listing
   if (final) {
      DriveChild uploaded;
      memSet(&uploaded, 0, sizeof uploaded);
      if (getJsonString(responseBuffer, length, "id", uploaded.id, sizeof uploaded.id)) {
         strCopy(uploaded.name, sizeof uploaded.name, state->fileName);
         uploaded.size  = state->uploaded;
         uploaded.mtime = parseModifiedTime(responseBuffer, length);
         putCachedChild(state->parentPath, &uploaded);
      }
   }
   return 0;
}

// section: VfsOps

static FileState *getOpenFile(VfsFile *file)
{
   if (file->descriptor < 0 || file->descriptor >= MAX_OPEN_FILES) return NULL;
   FileState *state = &filePool[file->descriptor];
   return state->used ? state : NULL;
}

static int statGdriveImpl(const char *native, VfsStat *outStat)
{
   memSet(outStat, 0, sizeof *outStat);
   const char *path = getDrivePath(native);
   if (path[0] == '/' && path[1] == '\0') { outStat->isDir = 1; outStat->mode = 0755; return 0; }   // mount root

   DriveChild *child = lookupChild(path);
   if (!child) return -1;

   outStat->isDir = child->isDir;
   outStat->size  = child->size;
   outStat->mtime = child->mtime;
   outStat->mode  = child->isDir ? 0755 : 0644;
   return 0;
}

static int openGdriveDirImpl(const char *native, VfsDir *dir)
{
   Listing *listing = ensureListing(getDrivePath(native));
   if (!listing) return -1;

   int slot = -1;
   for (int i = 0; i < MAX_OPEN_DIRS; i++) if (!dirPool[i].used) { slot = i; break; }
   if (slot < 0) return -1;

   // snapshot the children so readdir is unaffected if the cache slot is later refetched/evicted
   DirState *state = &dirPool[slot];
   state->count  = listing->count;
   state->cursor = 0;
   state->children = malloc(sizeof(DriveChild) * (listing->count > 0 ? listing->count : 1));
   if (!state->children) return -1;
   memCopy(state->children, listing->children, sizeof(DriveChild) * listing->count);
   state->used = 1;

   dir->descriptor  = slot;
   dir->nativeHandle = 0;
   return 0;
}

static int readGdriveDirImpl(VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut)
{
   if (dir->descriptor < 0 || dir->descriptor >= MAX_OPEN_DIRS || !dirPool[dir->descriptor].used) return -1;
   DirState *state = &dirPool[dir->descriptor];
   if (state->cursor >= state->count) return 0;

   DriveChild *child = &state->children[state->cursor++];
   strCopy(nameOut, nameCapacity, child->name);
   if (typeOut) *typeOut = child->isDir ? VFS_ENTRY_DIR : VFS_ENTRY_FILE;
   return 1;
}

static void closeGdriveDirImpl(VfsDir *dir)
{
   if (dir->descriptor >= 0 && dir->descriptor < MAX_OPEN_DIRS && dirPool[dir->descriptor].used) {
      free(dirPool[dir->descriptor].children);
      dirPool[dir->descriptor].children = NULL;
      dirPool[dir->descriptor].used = 0;
   }
   dir->descriptor = -1;
}

static int openGdriveImpl(const char *native, int flags, VfsFile *file)
{
   const char *path = getDrivePath(native);
   int writing = (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC)) != 0;

   // Drive has no partial write: a file is uploaded whole, so appending to (or editing part of) an
   // existing file is not something this backend can honour - better to refuse than to truncate it.
   if (flags & VFS_O_APPEND) return -1;
   if ((flags & VFS_O_RDWR) && !(flags & VFS_O_TRUNC)) return -1;

   int slot = -1;
   for (int i = 0; i < MAX_OPEN_FILES; i++) if (!filePool[i].used) { slot = i; break; }
   if (slot < 0) return -1;
   FileState *state = &filePool[slot];
   memSet(state, 0, sizeof *state);

   if (writing) {
      state->chunk = malloc(UPLOAD_CHUNK_BYTES);
      if (!state->chunk) return -1;
      state->chunkCapacity = UPLOAD_CHUNK_BYTES;
      if (startUploadSession(state, path) != 0) { free(state->chunk); state->chunk = NULL; return -1; }
      state->writing = 1;
   } else {
      DriveChild *child = lookupChild(path);
      if (!child || child->isDir) return -1;

      // a Doc/Sheet/Slide has no bytes to download (Drive answers 403 and reports no size), so opening
      // one for reading would hand back an empty file that a copy - or worse, a move - calls a success
      if (child->isGoogleNative) {
         logWarn("[gdrive] \"%s\" is a Google document, which has no file to copy\n", child->name);
         return -1;
      }
      strCopy(state->id, sizeof state->id, child->id);
      state->size = child->size;
   }

   state->used = 1;
   file->descriptor = slot;
   return 0;
}

// downloads the bytes at fileOffset into the bounce buffer, sizing it to wanted (clamped). 0 on success.
static int fetchChunk(FileState *state, uint64_t fileOffset, uint64_t wanted)
{
   if (wanted < CHUNK_MIN_BYTES) wanted = CHUNK_MIN_BYTES;
   if (wanted > CHUNK_MAX_BYTES) wanted = CHUNK_MAX_BYTES;
   uint64_t remaining = state->size - fileOffset;
   if (wanted > remaining) wanted = remaining;
   if (wanted == 0) return -1;

   if ((int)wanted > state->chunkCapacity) {
      char *grown = realloc(state->chunk, (size_t)wanted + 1);
      if (!grown) return -1;
      state->chunk         = grown;
      state->chunkCapacity = (int)wanted;
   }

   char url[64 + GDRIVE_ID_CAP];
   buildDriveFileUrl(url, sizeof url, API_FILES, state->id, "?alt=media");

   char range[64];
   int r = 0;
   appendStr(range, sizeof range, &r, "bytes=");
   r = appendUint64(range, sizeof range, r, fileOffset);
   appendStr(range, sizeof range, &r, "-");
   r = appendUint64(range, sizeof range, r, fileOffset + wanted - 1);
   range[r] = '\0';

   DriveRequest request = { .method = "GET", .url = url, .range = range,
                            .responseBuffer = state->chunk, .responseCapacity = state->chunkCapacity + 1 };
   int got = sendDriveRequest(&request);

   // 206 is the ranged answer; a 200 means the range was ignored and these are the file's FIRST bytes,
   // which must not be filed away as though they came from fileOffset
   int ok = got > 0 && (request.status == 206 || (request.status == 200 && fileOffset == 0));
   if (!ok) {
      logError("[gdrive] download status=%d range=%s got=%d body=%s\n", request.status, range, got,
               got > 0 ? state->chunk : "");
      state->chunkLength = 0;
      return -1;
   }
   state->chunkStart  = fileOffset;
   state->chunkLength = got;
   return 0;
}

static int64_t readGdriveImpl(VfsFile *file, void *buffer, uint64_t length)
{
   FileState *state = getOpenFile(file);
   if (!state || state->writing) return -1;
   if (length == 0 || state->position >= state->size) return 0;

   uint64_t remaining = state->size - state->position;
   if (length > remaining) length = remaining;

   char *out = (char *)buffer;
   uint64_t done = 0;
   while (done < length) {
      uint64_t offsetInChunk = state->position - state->chunkStart;
      int outsideChunk = state->chunkLength <= 0 || state->position < state->chunkStart
                       || offsetInChunk >= (uint64_t)state->chunkLength;
      if (outsideChunk) {
         if (fetchChunk(state, state->position, length - done) != 0) return done ? (int64_t)done : -1;
         offsetInChunk = 0;
      }
      uint64_t available = (uint64_t)state->chunkLength - offsetInChunk;
      uint64_t take = (length - done < available) ? length - done : available;
      memCopy(out + done, state->chunk + offsetInChunk, (int)take);
      state->position += take;
      done += take;
   }
   return (int64_t)done;
}

static int64_t writeGdriveImpl(VfsFile *file, const void *buffer, uint64_t length)
{
   FileState *state = getOpenFile(file);
   if (!state || !state->writing) return -1;

   const char *in = (const char *)buffer;
   uint64_t done = 0;
   while (done < length) {
      if (state->chunkLength == state->chunkCapacity && uploadChunk(state, 0) != 0) return -1;
      uint64_t space = (uint64_t)(state->chunkCapacity - state->chunkLength);
      uint64_t take = (length - done < space) ? length - done : space;
      memCopy(state->chunk + state->chunkLength, in + done, (int)take);
      state->chunkLength += (int)take;
      state->position    += take;
      done += take;
   }
   return (int64_t)length;
}

static int64_t seekGdriveImpl(VfsFile *file, int64_t offset, int whence)
{
   FileState *state = getOpenFile(file);
   if (!state || state->writing) return -1;   // an upload only ever moves forward, by writing

   int64_t base = whence == VFS_SEEK_SET ? 0 : whence == VFS_SEEK_CUR ? (int64_t)state->position
                : whence == VFS_SEEK_END ? (int64_t)state->size : -1;
   if (base < 0 && whence != VFS_SEEK_SET) return -1;
   int64_t target = base + offset;
   if (target < 0) target = 0;
   if ((uint64_t)target > state->size) target = (int64_t)state->size;
   state->position = (uint64_t)target;
   return target;
}

static int closeGdriveImpl(VfsFile *file)
{
   FileState *state = getOpenFile(file);
   file->descriptor = -1;
   if (!state) return 0;

   int rc = 0;
   if (state->writing) {
      // an abandoned upload is never finalised: leaving the session unfinished keeps whatever the file
      // was before, where declaring the bytes we managed to send would commit a truncated version
      if (file->abandoned) logWarn("[gdrive] upload of \"%s\" abandoned; leaving Drive unchanged\n", state->fileName);
      else                 rc = uploadChunk(state, 1);
   }
   free(state->chunk);
   state->chunk = NULL;
   state->used  = 0;
   return rc;
}

// Drive reports a quota in bytes; an account with no limit (Workspace "unlimited") reports none, and
// we hand back a nominal 1 TB free so a copy into Drive isn't refused for want of a number. the free
// space widget re-asks every 10 seconds, so the answer is cached - a request per tick would stall
// browsing (this runs under the same lock).
static uint64_t quotaLimit, quotaUsage, quotaFetchedAt;

static int getFreeGdriveImpl(const char *native, uint64_t *freeBytes, uint64_t *totalBytes)
{
   (void)native;
   uint64_t now = sys_time_get_system_time();
   if (!quotaFetchedAt || now - quotaFetchedAt >= QUOTA_TTL_US) {
      char responseBuffer[HTTP_RESPONSE_CAP];
      DriveRequest request = { .method = "GET", .url = API_ABOUT,
                               .responseBuffer = responseBuffer, .responseCapacity = sizeof responseBuffer };
      int length = sendDriveRequest(&request);
      if (length < 0 || request.status != 200) {
         logError("[gdrive] about status=%d body=%s\n", request.status, length > 0 ? responseBuffer : "");
         return -1;
      }
      char limitText[32], usageText[32];
      quotaUsage = getJsonString(responseBuffer, length, "usage", usageText, sizeof usageText)
                 ? parseU64(usageText) : 0;
      quotaLimit = getJsonString(responseBuffer, length, "limit", limitText, sizeof limitText)
                 ? parseU64(limitText) : quotaUsage + (1ULL << 40);
      quotaFetchedAt = now;
   }

   if (totalBytes) *totalBytes = quotaLimit;
   if (freeBytes)  *freeBytes  = quotaLimit > quotaUsage ? quotaLimit - quotaUsage : 0;
   return 0;
}

// locked wrappers: every stateful op takes gdriveLock for its whole body (see the pools above)
static int statGdriveOp(const char *native, VfsStat *outStat)
{ lockGdrive(); int rc = statGdriveImpl(native, outStat); unlockGdrive(); return rc; }

static int openGdriveDirOp(const char *native, VfsDir *dir)
{ lockGdrive(); int rc = openGdriveDirImpl(native, dir); unlockGdrive(); return rc; }

static int readGdriveDirOp(VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut)
{ lockGdrive(); int rc = readGdriveDirImpl(dir, nameOut, nameCapacity, typeOut); unlockGdrive(); return rc; }

static void closeGdriveDirOp(VfsDir *dir)
{ lockGdrive(); closeGdriveDirImpl(dir); unlockGdrive(); }

static int openGdriveOp(const char *native, int flags, VfsFile *file)
{ lockGdrive(); int rc = openGdriveImpl(native, flags, file); unlockGdrive(); return rc; }

static int64_t readGdriveOp(VfsFile *file, void *buffer, uint64_t length)
{ lockGdrive(); int64_t rc = readGdriveImpl(file, buffer, length); unlockGdrive(); return rc; }

static int64_t writeGdriveOp(VfsFile *file, const void *buffer, uint64_t length)
{ lockGdrive(); int64_t rc = writeGdriveImpl(file, buffer, length); unlockGdrive(); return rc; }

static int64_t seekGdriveOp(VfsFile *file, int64_t offset, int whence)
{ lockGdrive(); int64_t rc = seekGdriveImpl(file, offset, whence); unlockGdrive(); return rc; }

static int closeGdriveOp(VfsFile *file)
{ lockGdrive(); int rc = closeGdriveImpl(file); unlockGdrive(); return rc; }

static int mkdirGdriveOp(const char *native)
{ lockGdrive(); int rc = mkdirGdriveImpl(native); unlockGdrive(); return rc; }

static int removeGdriveOp(const char *native)
{ lockGdrive(); int rc = removeGdriveImpl(native); unlockGdrive(); return rc; }

static int renameGdriveOp(const char *from, const char *to)
{ lockGdrive(); int rc = renameGdriveImpl(from, to); unlockGdrive(); return rc; }

static int getFreeGdriveOp(const char *native, uint64_t *freeBytes, uint64_t *totalBytes)
{ lockGdrive(); int rc = getFreeGdriveImpl(native, freeBytes, totalBytes); unlockGdrive(); return rc; }

// every chunk is PUT as it fills, so there is nothing buffered to flush here
static int fsyncGdriveOp(VfsFile *file) { (void)file; return 0; }

static const VfsOps gdriveOps = {
   statGdriveOp, renameGdriveOp, mkdirGdriveOp, removeGdriveOp, removeGdriveOp, getFreeGdriveOp,
   openGdriveDirOp, readGdriveDirOp, closeGdriveDirOp,
   openGdriveOp, readGdriveOp, writeGdriveOp, seekGdriveOp, fsyncGdriveOp, closeGdriveOp,
};

// section: lifecycle

void initGdrive(const char *settingsFilePath)
{
   mounted = 0;
   strCopy(settingsPath, sizeof settingsPath, settingsFilePath);

   char text[SETTINGS_TEXT_CAP];
   if (loadSettingsFile(settingsPath, "", text, sizeof text) < 0) {
      logWarn("[gdrive] could not read settings at %s\n", settingsPath);
      return;
   }
   loadCredentials(text);   // pasted plaintext keys first, else the console-bound blob

   // seed empty credential keys on first run so settings.txt documents them for the user to fill
   // (from the PC sign-in helper: dev/tools/get-gdrive-token.ps1)
   if (findSettingValue(text, "google_auth_enc") == NULL && findSettingValue(text, "google_client_id") == NULL) {
      upsertSettingValue(settingsPath, "google_client_id", "");
      upsertSettingValue(settingsPath, "google_client_secret", "");
      upsertSettingValue(settingsPath, "google_refresh_token", "");
      logInfo("[gdrive] seeded google credential keys in %s\n", settingsPath);
   }

   // empty keys mean the user isn't using Drive: no mount, no folder at the root. the sign-in itself
   // still waits until the folder is opened, so a configured mount costs nothing until it is used.
   if (!isGdriveConfigured()) {
      logInfo("[gdrive] no credentials in %s; Google Drive stays hidden\n", settingsPath);
      return;
   }

   // the lock guards every op, so without it the mount must not go live at all
   if (createLock(&gdriveLock) != 0) {
      logError("[gdrive] could not create the backend lock; Google Drive not available\n");
      return;
   }

   int mountRc = addVfsMount(GDRIVE_SEGMENT, GDRIVE_NATIVE, GDRIVE_SEGMENT, VFS_SCHEME_GDRIVE,
                             VFS_MOUNT_REMOTE, &gdriveOps);
   if (mountRc != 0) {
      logError("[gdrive] addVfsMount failed; Google Drive not available\n");
      return;
   }
   mounted = 1;
   logInfo("[gdrive] mounted /%s (authorized=%d)\n", GDRIVE_SEGMENT, isGdriveAuthorized());
}

void shutdownGdrive(void)
{
   if (mounted) { removeVfsMount(GDRIVE_SEGMENT); mounted = 0; }
   for (int i = 0; i < LISTING_CACHE_SLOTS; i++) {
      free(listingCache[i].children);
      listingCache[i].children = NULL;
      listingCache[i].fetchedAt = 0;
   }
   for (int i = 0; i < MAX_OPEN_DIRS; i++) {
      free(dirPool[i].children);
      dirPool[i].children = NULL;
      dirPool[i].used = 0;
   }
}

int isGdriveAuthorized(void) { return clientId[0] && clientSecret[0] && refreshToken[0]; }

int isGdrivePath(const char *path)
{
   if (path[0] != '/') return 0;
   const char *segment = path + 1;
   int n = 0;
   while (GDRIVE_SEGMENT[n] && segment[n] == GDRIVE_SEGMENT[n]) n++;
   return GDRIVE_SEGMENT[n] == '\0' && (segment[n] == '\0' || segment[n] == '/');
}
