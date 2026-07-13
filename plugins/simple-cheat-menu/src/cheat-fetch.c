#include "cheat-fetch.h"
#include "cheat-sync.h"         // syncMode, isGameTitleId, buildCheatPath
#include "string-utilities.h"   // appendStr, strCopy
#include "secret-token.h"       // CHEAT_SYNC_TOKEN (gitignored; compiled into the sprx)
#include "vsh.h"                // vshNotify
#include "thread.h"             // spawnThread, sleepMs, exitThread
#include "dbg.h"

#include <cell/http.h>
#include <cell/ssl.h>
#include <cell/rtc.h>           // cellRtcGetCurrentClockLocalTime: nonce entropy for the vote path

// the repo's console-ready cheat files: one GET of this url returns compiled/<titleId>.txt raw.
#define REPO_CONTENTS_URL "https://api.github.com/repos/mohasi/game-cheats/contents/compiled/"
#define FETCH_CAP         (64 * 1024)   // one 64KB page; cheat files are a few KB, far under this

extern void *overlayHeapAlloc(unsigned int size);   // 64KB-page heap, shared with the overlay
extern void  overlayHeapFree(void *ptr);
extern int   writeFile(const char *path, const char *data, unsigned long long len);
extern int   readFile(const char *path, char *buffer, int capacity);
extern int   overlayIsMenuVisible(void);            // suppress launch toasts while the menu is up

// the title being fetched right now (single fetch at a time, guarded by fetchInProgress).
// fetchIsUpdate picks the wording: launch-time "checking" vs user-pressed "updating".
static char         fetchTitleId[16];
static volatile int fetchInProgress;
static int          fetchIsUpdate;
static volatile int updateResult;   // UpdateResult for the menu thread (UPDATE_*), read-and-cleared by consumeUpdateResult

// accept whatever the console CA store already decided (github chains to a CA in firmware).
static int32_t verifySslCert(uint32_t verifyErr, CellSslCert const cert[], int certNum, const char *hostname, CellHttpSslId id, void *arg)
{
   (void)cert; (void)certNum; (void)hostname; (void)id; (void)arg;
   return verifyErr;
}

// one http request against a full url, reusing vsh's http/ssl stack (no init, no pools). method is a
// CELL_HTTP_METHOD_* token; accept sets the Accept header; a non-null token adds an Authorization
// header; a non-null body is sent as application/json. the response body (up to cap) lands in
// out/*outLen. returns the HTTP status code, or -1 on a transport failure. one fresh client per call,
// fully drained then destroyed — no keep-alive hazard.
static int httpRequest(const char *method, const char *url, const char *token, const char *accept, const char *body, int bodyLen, char *out, int cap, int *outLen)
{
   *outLen = 0;
   int status = -1;
   CellHttpClientId client = 0;
   CellHttpTransId  trans = 0;
   if (cellHttpCreateClient(&client) < 0 || !client) return -1;
   cellHttpClientSetSslCallback(client, verifySslCert, NULL);
   cellHttpClientSetConnTimeout(client, 10 * 1000 * 1000);
   cellHttpClientSetSendTimeout(client, 10 * 1000 * 1000);
   cellHttpClientSetRecvTimeout(client, 10 * 1000 * 1000);

   size_t poolSize = 0;
   if (cellHttpUtilParseUri(NULL, url, NULL, 0, &poolSize) < 0) goto cleanup;
   char uriPool[512];
   if (poolSize > sizeof uriPool) goto cleanup;
   CellHttpUri uri;
   if (cellHttpUtilParseUri(&uri, url, uriPool, poolSize, NULL) < 0) goto cleanup;
   if (cellHttpCreateTransaction(&trans, client, method, &uri) < 0) { trans = 0; goto cleanup; }

   // headers: always a User-Agent (github rejects requests without one); Accept + Authorization +
   // Content-Type as needed. the auth value ("token <pat>") is built into a local buffer.
   CellHttpHeader agent = { "User-Agent", "simple-cheat-menu" };
   cellHttpRequestAddHeader(trans, &agent);
   if (accept) { CellHttpHeader h = { "Accept", accept }; cellHttpRequestAddHeader(trans, &h); }
   char authValue[128];
   if (token) {
      int end = 0;
      appendStr(authValue, sizeof authValue, &end, "token ");
      appendStr(authValue, sizeof authValue, &end, token);
      authValue[end] = '\0';
      CellHttpHeader h = { "Authorization", authValue };
      cellHttpRequestAddHeader(trans, &h);
   }
   if (body) {
      CellHttpHeader contentType = { "Content-Type", "application/json" };
      cellHttpRequestAddHeader(trans, &contentType);
      cellHttpRequestSetContentLength(trans, (uint64_t)bodyLen);
   }

   size_t sent = 0;
   if (cellHttpSendRequest(trans, body, body ? (size_t)bodyLen : 0, &sent) < 0) goto cleanup;   // connect + TLS + send
   int code = 0;
   if (cellHttpResponseGetStatusCode(trans, &code) < 0) goto cleanup;
   status = code;

   // drain the body into out (stops at end-of-body, or when the buffer is full).
   int total = 0;
   while (total < cap) {
      size_t got = 0;
      int rc = cellHttpRecvResponse(trans, out + total, (size_t)(cap - total), &got);
      if (rc < 0) { status = -1; break; }   // transport failure (even mid-body): report it, so a truncated file isn't saved as complete
      if (got == 0) break;   // clean end of body
      total += (int)got;
   }
   *outLen = total;

cleanup:
   if (trans)  cellHttpDestroyTransaction(trans);
   if (client) cellHttpDestroyClient(client);
   return status;
}

// download this title's compiled cheat file and save it locally. returns 0 (saved), 1 (the
// repo has no file for this game -> 404), or -1 (error). retries transient failures (a cold
// dns resolver returns before it has an answer).
static int fetchCheatFile(const char *titleId)
{
   char url[160];
   int end = 0;
   appendStr(url, sizeof url, &end, REPO_CONTENTS_URL);
   appendStr(url, sizeof url, &end, titleId);
   appendStr(url, sizeof url, &end, ".txt");
   url[end] = '\0';

   char *body = (char *)overlayHeapAlloc(FETCH_CAP);
   if (!body) { logError("[cht] fetch: no heap page\n"); return -1; }

   int result = -1;
   for (int attempt = 1; attempt <= 3; attempt++) {
      int len = 0;
      int status = httpRequest(CELL_HTTP_METHOD_GET, url, NULL, "application/vnd.github.raw", NULL, 0, body, FETCH_CAP, &len);
      logInfo("[cht] fetch: %s attempt %d status=%d len=%d\n", titleId, attempt, status, len);
      if (status == 200) {
         if (len >= FETCH_CAP) { logError("[cht] fetch: body too big, not saving\n"); break; }   // truncated
         char path[128];
         buildCheatPath(path, sizeof path, titleId);
         result = writeFile(path, body, (unsigned long long)len) == 0 ? 0 : -1;
         logInfo("[cht] fetch: saved %s rc=%d\n", path, result);
         break;
      }
      if (status == 404) { result = 1; break; }   // no cheat for this game
      sleepMs(2000);   // transport/resolver blip - back off and retry
   }
   overlayHeapFree(body);
   return result;
}

// worker: fetch, then report the outcome. an Update hands the result to the menu thread (it shows
// an in-menu message, or toasts if the menu was closed). a launch fetch toasts, but only while the
// menu is closed (an open menu shows its own state). one-shot, self-exiting. \xEF\xA2\x92 = PS glyph.
static void fetchThread(uint64_t arg)
{
   (void)arg;
   if (!fetchIsUpdate && !overlayIsMenuVisible()) vshNotify("Checking for cheats...  ");
   int r = fetchCheatFile(fetchTitleId);
   if (fetchIsUpdate) {
      updateResult = (r == 0) ? UPDATE_SAVED : (r == 1) ? UPDATE_NOT_FOUND : UPDATE_ERROR;   // menu thread reacts
   } else if (!overlayIsMenuVisible()) {
      if (r == 0)      vshNotify("Cheats ready - press \xEF\xA2\x92  ");
      else if (r == 1) vshNotify("No cheats found for this game!  ");
      else             vshNotify("Couldn't check for cheats  ");
   }
   fetchInProgress = 0;
   exitThread();
}

// hand the blocking download to its own thread so the caller (the menu thread) never stalls.
static void startFetch(const char *titleId, int isUpdate)
{
   strCopy(fetchTitleId, sizeof fetchTitleId, titleId);
   fetchIsUpdate = isUpdate;
   fetchInProgress = 1;
   sys_ppu_thread_t tid;
   spawnThread(&tid, fetchThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "cheat-fetch");
}

void maybeFetchForGame(const char *titleId)
{
   if (fetchInProgress) return;
   if (syncMode == SYNC_OFFLINE) return;      // offline: never reach out
   if (!isGameTitleId(titleId)) return;       // homebrew/app: never has cheats

   // already local? nothing to fetch (a 1-byte read tells exists from missing).
   char path[128], probe[2];
   buildCheatPath(path, sizeof path, titleId);
   if (readFile(path, probe, sizeof probe) >= 0) return;

   startFetch(titleId, 0);
}

// user pressed Update (Triangle): always re-download and overwrite the local file (no local-exists
// skip). returns 1 if a fetch actually started, 0 if it was declined (already fetching / offline /
// not a game title) — the caller must only enter update mode when this returns 1, else the menu
// would be stuck in "Updating..." with no fetch to ever clear it.
int updateCheatsForGame(const char *titleId)
{
   if (fetchInProgress) return 0;
   if (syncMode == SYNC_OFFLINE) return 0;   // Update is hidden offline; guard anyway
   if (!isGameTitleId(titleId)) return 0;    // homebrew/app with a user-placed cheat file
   startFetch(titleId, 1);
   return 1;
}

// MENU thread: the Update outcome once, then clears. atomic read-and-clear so a fetch-thread
// write can't be lost in a read/clear gap (which would strand update mode).
UpdateResult consumeUpdateResult(void)
{
   return (UpdateResult)__sync_lock_test_and_set(&updateResult, 0);
}

// ===== vote (write) path: anonymous CHEAT_WORKED/CHEAT_FAILED feedback as a github PR =====

#define GH_API    "https://api.github.com/repos/mohasi/game-cheats"
#define GH_ACCEPT "application/vnd.github+json"

// the vote being sent (single vote at a time, guarded by voteInProgress). set by the menu thread
// before it spawns the worker; the worker only ever reads these copies, never live cheat state.
static char         voteTitleId[16];
static char         voteVersion[16];
static char         voteHashHex[9];    // 8 lowercase hex + NUL
static char         voteEventName[16]; // "CHEAT_WORKED" / "CHEAT_FAILED"
static char         voteBody[512];     // working-val lines for CHEAT_WORKED; empty for CHEAT_FAILED
static volatile int voteInProgress;
static volatile int voteResult;        // outcome for the menu thread: 0 none, 1 sent, 2 failed

// write value as 8 lowercase hex chars (no NUL) — the cheatHash and the nonce halves.
static void toHex8(char *out, unsigned int value)
{
   static const char hex[] = "0123456789abcdef";
   for (int i = 0; i < 8; i++) out[i] = hex[(value >> (28 - 4 * i)) & 0xF];
}

// first occurrence of needle in haystack, or 0. small local strstr (no libc in a vsh prx).
static const char *findSubstr(const char *haystack, const char *needle)
{
   for (const char *at = haystack; *at; at++) {
      const char *a = at, *b = needle;
      while (*a && *b && *a == *b) { a++; b++; }
      if (!*b) return at;
   }
   return 0;
}

// copy the string value of the first "key": "value" in json into out. returns 1 on success. just
// enough JSON to pull the commit sha out of the ref response — not a general parser.
static int findJsonString(const char *json, const char *key, char *out, int cap)
{
   out[0] = 0;
   char needle[32];
   int end = 0;
   appendStr(needle, sizeof needle, &end, "\"");
   appendStr(needle, sizeof needle, &end, key);
   appendStr(needle, sizeof needle, &end, "\"");
   needle[end] = 0;

   const char *at = findSubstr(json, needle);
   if (!at) return 0;
   at += end;
   while (*at && *at != ':') at++;
   if (*at != ':') return 0;
   at++;
   while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
   if (*at != '"') return 0;
   at++;
   int n = 0;
   while (*at && *at != '"' && n < cap - 1) out[n++] = *at++;
   out[n] = 0;
   return n > 0;
}

// standard base64 of in[0..inLen) into out (NUL-terminated). returns the length, or -1 if it wouldn't
// fit. the github contents API wants the file body base64-encoded.
static int base64Encode(const unsigned char *in, int inLen, char *out, int cap)
{
   static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   int o = 0;
   for (int i = 0; i < inLen; i += 3) {
      int remaining = inLen - i;
      unsigned int triple = (unsigned int)in[i] << 16
                          | (unsigned int)(remaining > 1 ? in[i + 1] : 0) << 8
                          | (unsigned int)(remaining > 2 ? in[i + 2] : 0);
      if (o + 4 > cap - 1) return -1;
      out[o++] = tbl[(triple >> 18) & 0x3F];
      out[o++] = tbl[(triple >> 12) & 0x3F];
      out[o++] = remaining > 1 ? tbl[(triple >> 6) & 0x3F] : '=';
      out[o++] = remaining > 2 ? tbl[triple & 0x3F] : '=';
   }
   out[o] = 0;
   return o;
}

// a fresh 16-hex nonce for the vote path/branch: microsecond entropy + clock fields + a rolling salt,
// so two votes in the same microsecond still differ. anonymous — no console id, purely path uniqueness.
static unsigned int voteNonceSalt = 0;
static void buildNonce(char *out)   // out[17]
{
   CellRtcDateTime now;
   unsigned int microPart = 0, clockPart = 0;
   if (cellRtcGetCurrentClockLocalTime(&now) == 0) {
      microPart = (unsigned int)now.microsecond;
      clockPart = (unsigned int)now.second | ((unsigned int)now.minute << 6) | ((unsigned int)now.hour << 12) | ((unsigned int)now.day << 17);
   }
   clockPart ^= (++voteNonceSalt) << 24;
   toHex8(out, microPart);
   toHex8(out + 8, clockPart);
   out[16] = 0;
}

// log a failed call with a short snippet of the response body, so a first failure pinpoints the cause
// (401 bad token, 403 scope/rate, 422 validation, ...). github error bodies never contain the token.
static void logVoteFail(const char *stage, int status, char *response, int len)
{
   if (len < 0) len = 0;
   if (len > 180) len = 180;
   response[len] = 0;
   logError("[cht] vote: %s FAILED status=%d body=%s\n", stage, status, response);
}

// the 4-call github flow for one vote: GET main sha -> POST vote branch -> PUT vote file -> POST PR.
// validate-votes then auto-merges it. returns 0 on success (PR opened), -1 otherwise. logs every step
// so a failure lands on exactly one call. runs on the worker thread.
static int uploadVote(void)
{
   char response[1024];
   char nonce[17];
   buildNonce(nonce);
   logInfo("[cht] vote: sending %s ps3 %s/%s/%s\n", voteEventName, voteTitleId, voteVersion, voteHashHex);

   char branch[40];
   int end = 0;
   appendStr(branch, sizeof branch, &end, "vote-");
   appendStr(branch, sizeof branch, &end, nonce);
   branch[end] = 0;

   // 1. main's head sha (retry the cold-connection/dns blip a couple times)
   int status = -1, len = 0;
   for (int attempt = 0; attempt < 3 && status <= 0; attempt++) {
      status = httpRequest(CELL_HTTP_METHOD_GET, GH_API "/git/ref/heads/main", CHEAT_SYNC_TOKEN, GH_ACCEPT, NULL, 0, response, sizeof response - 1, &len);
      if (status <= 0) sleepMs(2000);
   }
   logInfo("[cht] vote: 1/4 GET ref status=%d\n", status);
   if (status != 200) { logVoteFail("GET ref", status, response, len); return -1; }
   response[len] = 0;
   char sha[64];
   if (!findJsonString(response, "sha", sha, sizeof sha)) { logError("[cht] vote: no sha in ref response\n"); return -1; }

   // 2. create the vote branch at that sha
   char body[1400];
   end = 0;
   appendStr(body, sizeof body, &end, "{\"ref\":\"refs/heads/");
   appendStr(body, sizeof body, &end, branch);
   appendStr(body, sizeof body, &end, "\",\"sha\":\"");
   appendStr(body, sizeof body, &end, sha);
   appendStr(body, sizeof body, &end, "\"}");
   body[end] = 0;
   status = httpRequest(CELL_HTTP_METHOD_POST, GH_API "/git/refs", CHEAT_SYNC_TOKEN, GH_ACCEPT, body, end, response, sizeof response - 1, &len);
   logInfo("[cht] vote: 2/4 POST branch status=%d\n", status);
   if (status != 201) { logVoteFail("POST branch", status, response, len); return -1; }

   // 3. put the vote file on the branch — the path IS the vote; the body (base64) carries working-val
   char path[192];
   end = 0;
   appendStr(path, sizeof path, &end, "votes/");
   appendStr(path, sizeof path, &end, voteTitleId);
   appendStr(path, sizeof path, &end, "/");
   appendStr(path, sizeof path, &end, voteVersion);
   appendStr(path, sizeof path, &end, "/");
   appendStr(path, sizeof path, &end, voteHashHex);
   appendStr(path, sizeof path, &end, "/");
   appendStr(path, sizeof path, &end, voteEventName);
   appendStr(path, sizeof path, &end, "-ps3-");
   appendStr(path, sizeof path, &end, nonce);
   path[end] = 0;

   char contentB64[768];
   if (base64Encode((const unsigned char *)voteBody, getStrLen(voteBody), contentB64, sizeof contentB64) < 0) { logError("[cht] vote: body too big to encode\n"); return -1; }

   char putUrl[256];
   end = 0;
   appendStr(putUrl, sizeof putUrl, &end, GH_API "/contents/");
   appendStr(putUrl, sizeof putUrl, &end, path);
   putUrl[end] = 0;

   end = 0;
   appendStr(body, sizeof body, &end, "{\"message\":\"");
   appendStr(body, sizeof body, &end, voteEventName);
   appendStr(body, sizeof body, &end, " ");
   appendStr(body, sizeof body, &end, voteTitleId);
   appendStr(body, sizeof body, &end, " ");
   appendStr(body, sizeof body, &end, voteVersion);
   appendStr(body, sizeof body, &end, "\",\"content\":\"");
   appendStr(body, sizeof body, &end, contentB64);
   appendStr(body, sizeof body, &end, "\",\"branch\":\"");
   appendStr(body, sizeof body, &end, branch);
   appendStr(body, sizeof body, &end, "\"}");
   body[end] = 0;
   status = httpRequest(CELL_HTTP_METHOD_PUT, putUrl, CHEAT_SYNC_TOKEN, GH_ACCEPT, body, end, response, sizeof response - 1, &len);
   logInfo("[cht] vote: 3/4 PUT file status=%d\n", status);
   if (status != 201) { logVoteFail("PUT file", status, response, len); return -1; }

   // 4. open the PR — validate-votes auto-merges it
   end = 0;
   appendStr(body, sizeof body, &end, "{\"title\":\"");
   appendStr(body, sizeof body, &end, voteEventName);
   appendStr(body, sizeof body, &end, " ");
   appendStr(body, sizeof body, &end, voteTitleId);
   appendStr(body, sizeof body, &end, " ");
   appendStr(body, sizeof body, &end, voteVersion);
   appendStr(body, sizeof body, &end, "\",\"head\":\"");
   appendStr(body, sizeof body, &end, branch);
   appendStr(body, sizeof body, &end, "\",\"base\":\"main\"}");
   body[end] = 0;
   status = httpRequest(CELL_HTTP_METHOD_POST, GH_API "/pulls", CHEAT_SYNC_TOKEN, GH_ACCEPT, body, end, response, sizeof response - 1, &len);
   logInfo("[cht] vote: 4/4 POST pr status=%d\n", status);
   if (status != 201) { logVoteFail("POST pr", status, response, len); return -1; }

   logInfo("[cht] vote: PR opened, awaiting auto-merge\n");
   return 0;
}

// client-side dedup: votes are anonymous, so the repo can't tell two votes from one console apart —
// this console remembers what it has already uploaded so it never sends the same vote twice. one
// "titleId version cheatHash event" line per uploaded vote, in the plugin data dir.
#define SENT_VOTES_PATH PLUGIN_DIR "/votes-sent.txt"

static void buildVoteKey(char *out, int cap, const char *titleId, const char *version, const char *hashHex, const char *eventName)
{
   int end = 0;
   appendStr(out, cap, &end, titleId);  appendStr(out, cap, &end, " ");
   appendStr(out, cap, &end, version);  appendStr(out, cap, &end, " ");
   appendStr(out, cap, &end, hashHex);  appendStr(out, cap, &end, " ");
   appendStr(out, cap, &end, eventName);
   out[end] = 0;
}

// has this exact vote already been uploaded from this console? fails open (allows the vote) if the
// file can't be read — a rare duplicate beats silently dropping a real vote.
static int voteAlreadySent(const char *key)
{
   char *buffer = (char *)overlayHeapAlloc(FETCH_CAP);
   if (!buffer) return 0;
   int bytes = readFile(SENT_VOTES_PATH, buffer, FETCH_CAP - 1);
   int found = 0;
   if (bytes > 0) { buffer[bytes] = 0; found = findSubstr(buffer, key) != 0; }
   overlayHeapFree(buffer);
   return found;
}

// remember an uploaded vote so it is never sent again (append the key line, read-modify-write).
static void recordVoteSent(const char *key)
{
   char *buffer = (char *)overlayHeapAlloc(FETCH_CAP);
   if (!buffer) return;
   int bytes = readFile(SENT_VOTES_PATH, buffer, FETCH_CAP - 128);
   if (bytes < 0) bytes = 0;
   int end = bytes;
   appendStr(buffer, FETCH_CAP, &end, key);
   appendStr(buffer, FETCH_CAP, &end, "\n");
   writeFile(SENT_VOTES_PATH, buffer, (unsigned long long)end);
   overlayHeapFree(buffer);
}

static void voteThread(uint64_t arg)
{
   (void)arg;
   int sent = uploadVote() == 0;
   if (sent) {
      char key[64];
      buildVoteKey(key, sizeof key, voteTitleId, voteVersion, voteHashHex, voteEventName);
      recordVoteSent(key);   // never upload this same vote again
   }
   voteResult = sent ? 1 : 2;   // 1 sent, 2 failed — the menu thread shows the outcome
   voteInProgress = 0;
   exitThread();
}

// MENU thread: the vote outcome once, then clears (atomic) — 0 none, 1 sent, 2 failed.
int consumeVoteResult(void)
{
   return __sync_lock_test_and_set(&voteResult, 0);
}

// MENU thread: send one anonymous vote for a cheat as a github PR (branch + file + PR; the workflow
// validates and auto-merges). copies its inputs and hands the blocking network to a worker so the
// caller never stalls. returns 1 if a vote started, 0 if declined (already sending / not contribute /
// bad inputs). the working-val body applies to CHEAT_WORKED only (empty otherwise).
int submitCheatVote(const char *titleId, const char *version, unsigned int cheatHash, VoteEvent event, const char *body)
{
   if (voteInProgress) return 0;
   if (syncMode != SYNC_CONTRIBUTE) return 0;
   if (!isGameTitleId(titleId)) return 0;
   if (getStrLen(version) < 4) return 0;   // need NN.NN for a valid vote path
   if (cheatHash == 0) return 0;

   char hashHex[9];
   toHex8(hashHex, cheatHash); hashHex[8] = 0;
   const char *eventName = event == VOTE_WORKED ? "CHEAT_WORKED" : "CHEAT_FAILED";

   char key[64];
   buildVoteKey(key, sizeof key, titleId, version, hashHex, eventName);
   if (voteAlreadySent(key)) return 2;   // this console already uploaded this vote — don't duplicate

   strCopy(voteTitleId, sizeof voteTitleId, titleId);
   strCopy(voteVersion, sizeof voteVersion, version);
   strCopy(voteHashHex, sizeof voteHashHex, hashHex);
   strCopy(voteEventName, sizeof voteEventName, eventName);
   strCopy(voteBody, sizeof voteBody, body ? body : "");

   voteInProgress = 1;
   sys_ppu_thread_t tid;
   spawnThread(&tid, voteThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "cheat-vote");
   return 1;
}
