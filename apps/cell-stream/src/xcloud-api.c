// xcloud-api - see xcloud-api.h.
//
// The server checks who is asking through the X-MS-Device-Info header as well as the token, so the
// headers below claim to be the Xbox web player. green-vita reaches the same servers from a Vita
// this way, so what they check is what is claimed rather than what is connecting.

#include "xcloud-api.h"

#include "cell-stream-settings.h"
#include "dbg.h"
#include "http.h"
#include "json.h"
#include "printf.h"
#include "string-utilities.h"
#include "thread.h"
#include "vfs.h"
#include "xcloud-auth.h"

#include <stdlib.h>

#define TAG "[cst] "

#define DEVICE_INFO "{\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\",\"clientAppType\":\"browser\"," \
   "\"clientAppVersion\":\"26.1.97\",\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\"," \
   "\"sdkInstallId\":\"\"}}," \
   "\"dev\":{\"hw\":{\"make\":\"Sony\",\"model\":\"PlayStation 3\",\"sdktype\":\"web\"},\"os\":{\"name\":\"android\"," \
   "\"ver\":\"22631.2715\",\"platform\":\"desktop\"},\"displayInfo\":{\"dimensions\":{\"widthInPixels\":1280," \
   "\"heightInPixels\":720},\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}},\"browser\":{\"browserName\":\"chrome\"," \
   "\"browserVersion\":\"140.0.3485.54\"}}}"

#define TITLES_PATH "/v2/titles"

// the whole catalogue arrives in one answer, playable or not: 1853 entries filled 768 KB and were
// still cut off, so this is sized well past that and a short read is reported rather than parsed.
#define TITLES_BODY_MAX (4 * 1024 * 1024)

#define PLAY_PATH "/v5/sessions/cloud/play"
#define SESSION_ANSWER_MAX  16384   // the connection description that comes back is several KB
#define PASSPORT_TOKEN_MAX  4096
#define DROPPED_POLLS_ALLOWED 5   // about five seconds of trying before a session is called lost

#define SDP_BODY_MAX      8192
#define SDP_ANSWER_TRIES  20      // the answer takes a moment; this is ten seconds of asking
#define ICE_ANSWER_TRIES  20
#define SDP_ANSWER_PATH   CELL_STREAM_SETTINGS_DIR "/sdp-answer.txt"

static XcloudTitle titles[TITLE_LIST_MAX];
static int titleCount;
static char apiError[128];

// the session token is a signed blob of a few KB, so the header carrying it cannot sit on the stack
static char bearerHeader[XCLOUD_STREAM_TOKEN_MAX + 8];

// an authorized request to the regional server. the caller owns `out`; returns the HTTP status,
// or -1 when nothing came back.
static int requestFromRegion(const char *method, const char *path, const char *body, char *out, int capacity,
                             int *outLength)
{
   char url[256];
   snprintf(url, sizeof url, "%s%s", getXcloudStreamHost(), path);
   snprintf(bearerHeader, sizeof bearerHeader, "Bearer %s", getXcloudStreamToken());

   const HttpHeader headers[] = {
      { "Authorization", bearerHeader },
      { "Accept", "application/json" },
      { "Content-Type", "application/json" },
      { "X-Gssv-Client", "XboxComBrowser" },
      { "X-MS-Device-Info", DEVICE_INFO },
   };

   int status = 0;
   if (fetchHttp(method, url, headers, 5, body, body ? getStrLen(body) : 0, out, capacity, outLength, &status) != 0)
      return -1;
   return status;
}

// a title counts as playable when the account owns it, or when one of the programs it belongs to is
// one the account subscribes to. anything else would fail at the play request.
static int isTitlePlayable(const char *details, int length)
{
   char entitlement[8];
   if (getJsonText(details, length, "hasEntitlement", entitlement, sizeof entitlement) == 0 &&
       strEq(entitlement, "true"))
      return 1;

   int programsStart = 0, programsEnd = 0;
   if (findJsonArray(details, length, "programs", &programsStart, &programsEnd) != 0) return 0;

   int subscriptionsStart = 0, subscriptionsEnd = 0;
   if (findJsonArray(details, length, "userSubscriptions", &subscriptionsStart, &subscriptionsEnd) != 0) return 0;

   // both are short lists of quoted names, so a substring match over the subscriptions is enough
   for (int at = programsStart; at < programsEnd; at++) {
      if (details[at] != '"') continue;
      int nameStart = at + 1, nameEnd = nameStart;
      while (nameEnd < programsEnd && details[nameEnd] != '"') nameEnd++;
      if (findBytes(details + subscriptionsStart, subscriptionsEnd - subscriptionsStart, details + nameStart,
                    nameEnd - nameStart) >= 0)
         return 1;
      at = nameEnd;
   }
   return 0;
}

int fetchXcloudTitles(void)
{
   titleCount = 0;
   apiError[0] = 0;

   char *body = malloc(TITLES_BODY_MAX);
   if (!body) {
      strCopy(apiError, sizeof apiError, "not enough memory for the title list");
      return -1;
   }

   // ask for the catalogue
   int length = 0;
   int status = requestFromRegion("GET", TITLES_PATH, NULL, body, TITLES_BODY_MAX, &length);
   if (status != 200) {
      snprintf(apiError, sizeof apiError, "the title list was refused (%d)", status);
      logError(TAG "titles request failed, status=%d\n", status);
      free(body);
      return -1;
   }

   // a body that exactly fills the buffer was cut short, and half a list parses without complaining
   if (length >= TITLES_BODY_MAX - 1) {
      snprintf(apiError, sizeof apiError, "the title list is bigger than %d KB", TITLES_BODY_MAX / 1024);
      logError(TAG "titles answer filled the whole %d KB buffer, so it was truncated\n", TITLES_BODY_MAX / 1024);
      free(body);
      return -1;
   }

   int listStart = 0, listEnd = 0;
   if (findJsonArray(body, length, "results", &listStart, &listEnd) != 0) {
      strCopy(apiError, sizeof apiError, "the title list made no sense");
      logError(TAG "titles answer had no results array, %d bytes\n", length);
      free(body);
      return -1;
   }

   // keep the ones this account can actually start
   int skipped = 0, entryStart = 0, entryEnd = 0, offset = listStart;
   while (titleCount < TITLE_LIST_MAX &&
          (offset = readJsonObject(body, listEnd, offset, &entryStart, &entryEnd)) != 0) {
      const char *entry = body + entryStart;
      int entryLength = entryEnd - entryStart;

      XcloudTitle title;
      if (getJsonText(entry, entryLength, "titleId", title.id, sizeof title.id) != 0) continue;

      int detailsStart = 0, detailsEnd = 0;
      if (getJsonObject(entry, entryLength, "details", &detailsStart, &detailsEnd) != 0) continue;

      const char *details = entry + detailsStart;
      int detailsLength = detailsEnd - detailsStart;
      if (!isTitlePlayable(details, detailsLength)) { skipped++; continue; }

      if (getJsonText(details, detailsLength, "productId", title.productId, sizeof title.productId) != 0)
         title.productId[0] = 0;

      titles[titleCount++] = title;
   }

   free(body);
   const XcloudTitle *first = getXcloudTitle(0);
   logInfo(TAG "titles: %d playable, %d not on this account (%d bytes), first is %s\n", titleCount, skipped, length,
           first ? first->id : "none");
   return titleCount;
}

int getXcloudTitleCount(void) { return titleCount; }

const XcloudTitle *getXcloudTitle(int index)
{
   return index >= 0 && index < titleCount ? &titles[index] : NULL;
}

const char *getXcloudApiError(void) { return apiError; }

// section: play sessions

// the session calls run one at a time, so they share these rather than putting KBs on the stack
static char sessionAnswer[SESSION_ANSWER_MAX];
static char passportToken[PASSPORT_TOKEN_MAX];
static char sdpAnswer[SDP_BODY_MAX];   // the description itself, out of its two wrappers

static XcloudSessionState sessionState = XCLOUD_SESSION_NONE;
static char sessionPath[192];
static int handedOver;    // the connect step below runs once, the first time the session reports ready
static int droppedPolls;  // state requests in a row that never reached the server

static XcloudSessionState stateFromName(const char *name)
{
   if (strEq(name, "Provisioning")) return XCLOUD_SESSION_PROVISIONING;
   if (strEq(name, "WaitingForResources")) return XCLOUD_SESSION_WAITING;
   if (strEq(name, "ReadyToConnect")) return XCLOUD_SESSION_AUTHORIZING;
   if (strEq(name, "Provisioned")) return XCLOUD_SESSION_READY;
   if (strEq(name, "Failed") || strEq(name, "Error")) return XCLOUD_SESSION_FAILED;
   return XCLOUD_SESSION_PROVISIONING;   // a state we do not know is still one to keep waiting through
}

static int failSession(const char *reason)
{
   strCopy(apiError, sizeof apiError, reason);
   sessionState = XCLOUD_SESSION_FAILED;
   logError(TAG "session: %s\n", reason);
   return -1;
}

int startXcloudSession(const char *titleId)
{
   sessionState = XCLOUD_SESSION_NONE;
   sessionPath[0] = 0;
   handedOver = 0;
   droppedPolls = 0;
   apiError[0] = 0;

   char body[1024];
   snprintf(body, sizeof body,
            "{\"clientSessionId\":\"\",\"titleId\":\"%s\",\"systemUpdateGroup\":\"\","
            "\"settings\":{\"nanoVersion\":\"V3;WebrtcTransport.dll\",\"enableOptionalDataCollection\":false,"
            "\"enableTextToSpeech\":false,\"highContrast\":0,\"locale\":\"en-GB\",\"useIceConnection\":false,"
            "\"timezoneOffsetMinutes\":0,\"sdkType\":\"web\",\"osName\":\"android\"},"
            "\"serverId\":\"\",\"fallbackRegionNames\":[]}",
            titleId);

   int length = 0;
   int status = requestFromRegion("POST", PLAY_PATH, body, sessionAnswer, sizeof sessionAnswer, &length);
   if (status != 200 && status != 202) {
      snprintf(apiError, sizeof apiError, "the server would not start that title (%d)", status);
      logError(TAG "play request failed, status=%d, answer=%.200s\n", status, sessionAnswer);
      sessionState = XCLOUD_SESSION_FAILED;
      return -1;
   }

   // the server answers with a path that has no leading slash, and every later request builds a url
   // by pasting it onto the host, so put one back before it is used
   char reported[sizeof sessionPath];
   if (getJsonText(sessionAnswer, length, "sessionPath", reported, sizeof reported) != 0)
      return failSession("the server started a session but did not say where");
   snprintf(sessionPath, sizeof sessionPath, "/%s", reported[0] == '/' ? reported + 1 : reported);

   sessionState = XCLOUD_SESSION_PROVISIONING;
   logInfo(TAG "session %s starting for title %s\n", sessionPath, titleId);
   return 0;
}

// the handover the server expects the moment a session first reports ready
static int connectToSession(void)
{
   if (fetchXcloudPassportToken(passportToken, sizeof passportToken) != 0)
      return failSession("could not get the token the session asked for");

   char *body = malloc(PASSPORT_TOKEN_MAX + 64);
   if (!body) return failSession("not enough memory to connect");
   snprintf(body, PASSPORT_TOKEN_MAX + 64, "{\"userToken\":\"%s\"}", passportToken);

   char path[256];
   snprintf(path, sizeof path, "%s/connect", sessionPath);

   int length = 0;
   int status = requestFromRegion("POST", path, body, sessionAnswer, sizeof sessionAnswer, &length);
   free(body);

   if (status != 200 && status != 202) {
      snprintf(apiError, sizeof apiError, "the session refused the handover (%d)", status);
      logError(TAG "session connect failed, status=%d\n", status);
      sessionState = XCLOUD_SESSION_FAILED;
      return -1;
   }

   logInfo(TAG "session handover accepted\n");
   return 0;
}

XcloudSessionState pollXcloudSession(void)
{
   if (sessionState == XCLOUD_SESSION_NONE || sessionState == XCLOUD_SESSION_FAILED) return sessionState;

   char path[256], name[64];
   snprintf(path, sizeof path, "%s/state", sessionPath);

   int length = 0;
   int status = requestFromRegion("GET", path, NULL, sessionAnswer, sizeof sessionAnswer, &length);

   // a dropped request is not an answer, so the next poll asks again. but silently retrying forever
   // hides a request that can never work, so a run of them ends the session.
   if (status < 0) {
      if (++droppedPolls < DROPPED_POLLS_ALLOWED) return sessionState;
      failSession("the session could not be reached");
      return sessionState;
   }
   droppedPolls = 0;
   if (status != 200) {
      snprintf(apiError, sizeof apiError, "the session stopped answering (%d)", status);
      logError(TAG "session state request failed, status=%d\n", status);
      sessionState = XCLOUD_SESSION_FAILED;
      return sessionState;
   }

   if (getJsonText(sessionAnswer, length, "state", name, sizeof name) != 0) return sessionState;

   XcloudSessionState reported = stateFromName(name);
   if (reported != sessionState) logInfo(TAG "session state %s\n", name);
   sessionState = reported;

   if (sessionState == XCLOUD_SESSION_AUTHORIZING && !handedOver) {
      handedOver = 1;
      connectToSession();
   }
   return sessionState;
}

XcloudSessionState getXcloudSessionState(void) { return sessionState; }

int exchangeXcloudSdp(const char *offer)
{
   if (sessionState != XCLOUD_SESSION_READY) return -1;

   // the offer is carried as a JSON string, so its newlines have to be escaped
   char *body = malloc(SDP_BODY_MAX);
   if (!body) return failSession("not enough memory to make the offer");

   int at = snprintf(body, SDP_BODY_MAX, "{\"messageType\":\"offer\",\"requestId\":\"1\",\"sdp\":\"");
   for (const char *from = offer; *from && at < SDP_BODY_MAX - 8; from++) {
      if (*from == '\n') { body[at++] = '\\'; body[at++] = 'n'; }
      else if (*from == '\r') continue;
      else if (*from == '"') { body[at++] = '\\'; body[at++] = '"'; }
      else body[at++] = *from;
   }
   at += snprintf(body + at, SDP_BODY_MAX - at,
                  "\",\"configuration\":{\"chatConfiguration\":{\"bytesPerSample\":2,"
                  "\"expectedClipDurationMs\":20,\"format\":{\"codec\":\"opus\",\"container\":\"webm\"},"
                  "\"numChannels\":1,\"sampleFrequencyHz\":24000},\"chat\":{\"minVersion\":1,\"maxVersion\":1},"
                  "\"control\":{\"minVersion\":1,\"maxVersion\":3},\"input\":{\"minVersion\":1,\"maxVersion\":9},"
                  "\"message\":{\"minVersion\":1,\"maxVersion\":1},"
                  "\"reliableinput\":{\"minVersion\":9,\"maxVersion\":9},"
                  "\"unreliableinput\":{\"minVersion\":9,\"maxVersion\":9}}}");

   char path[256];
   snprintf(path, sizeof path, "%s/sdp", sessionPath);

   int length = 0;
   int status = requestFromRegion("POST", path, body, sessionAnswer, sizeof sessionAnswer, &length);
   free(body);
   if (status != 200 && status != 202) {
      snprintf(apiError, sizeof apiError, "the offer was refused (%d)", status);
      logError(TAG "sdp offer failed, status=%d, answer=%.300s\n", status, sessionAnswer);
      return -1;
   }

   // the reply is not ready straight away; the server answers 204 until it is
   for (int tries = 0; tries < SDP_ANSWER_TRIES; tries++) {
      sleepMs(500);
      length = 0;
      status = requestFromRegion("GET", path, NULL, sessionAnswer, sizeof sessionAnswer, &length);
      if (status != 200) continue;
      if (findBytes(sessionAnswer, length, "\"status\":204", 12) >= 0) continue;

      makeDirPath(CELL_STREAM_SETTINGS_DIR);
      writeFile(SDP_ANSWER_PATH, sessionAnswer, length);

      // the description is wrapped twice: an object holding a string that itself holds it
      char wrapped[SDP_BODY_MAX];
      if (getJsonText(sessionAnswer, length, "exchangeResponse", wrapped, sizeof wrapped) != 0 ||
          getJsonText(wrapped, getStrLen(wrapped), "sdp", sdpAnswer, sizeof sdpAnswer) != 0) {
         strCopy(apiError, sizeof apiError, "the machine's answer held no description");
         logError(TAG "sdp answer could not be unwrapped, %d bytes\n", length);
         return -1;
      }

      logInfo(TAG "sdp answer saved, %d bytes\n", length);
      return 0;
   }

   strCopy(apiError, sizeof apiError, "the machine never answered the offer");
   logError(TAG "no sdp answer after %d tries\n", SDP_ANSWER_TRIES);
   return -1;
}

int postXcloudIceCandidate(const char *candidate)
{
   if (sessionState != XCLOUD_SESSION_READY) return -1;

   char body[512], path[256];
   snprintf(body, sizeof body,
            "{\"messageType\":\"iceCandidate\",\"candidate\":\"%s\",\"sdpMid\":\"0\",\"sdpMLineIndex\":0}", candidate);
   snprintf(path, sizeof path, "%s/ice", sessionPath);

   int length = 0;
   int status = requestFromRegion("POST", path, body, sessionAnswer, sizeof sessionAnswer, &length);
   if (status != 200 && status != 202) {
      snprintf(apiError, sizeof apiError, "the machine would not take our address (%d)", status);
      logError(TAG "ice post failed, status=%d, answer=%.200s\n", status, sessionAnswer);
      return -1;
   }

   // its own addresses follow, and like the description they are not ready straight away
   for (int tries = 0; tries < ICE_ANSWER_TRIES; tries++) {
      sleepMs(500);
      length = 0;
      status = requestFromRegion("GET", path, NULL, sessionAnswer, sizeof sessionAnswer, &length);
      if (status != 200) continue;
      if (findBytes(sessionAnswer, length, "\"status\":204", 12) >= 0) continue;

      logInfo(TAG "ice answer, %d bytes\n", length);
      return 0;
   }

   strCopy(apiError, sizeof apiError, "the machine never sent its address");
   logError(TAG "no ice answer after %d tries\n", ICE_ANSWER_TRIES);
   return -1;
}

const char *getXcloudIceAnswer(void) { return sessionAnswer; }
const char *getXcloudSdpAnswer(void) { return sdpAnswer; }

void stopXcloudSession(void)
{
   if (sessionPath[0]) {
      int length = 0;
      int status = requestFromRegion("DELETE", sessionPath, NULL, sessionAnswer, sizeof sessionAnswer, &length);
      if (status == 200 || status == 202 || status == 204) logInfo(TAG "session %s given back\n", sessionPath);
      else logWarn(TAG "session %s may still be running, the server answered %d\n", sessionPath, status);
   }
   sessionPath[0] = 0;
   sessionState = XCLOUD_SESSION_NONE;
   handedOver = 0;
   droppedPolls = 0;
}
