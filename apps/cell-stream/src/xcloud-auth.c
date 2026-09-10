// xcloud-auth - see xcloud-auth.h.
//
// The client id and scope are the ones the Xbox web player uses; Microsoft ties the device-code
// flow to a registered application and will not issue a code for an unknown one.

#include "xcloud-auth.h"

#include "cell-stream-settings.h"
#include "dbg.h"
#include "http.h"
#include "json.h"
#include "printf.h"
#include "string-utilities.h"
#include "vfs.h"

#define TAG "[cst] "

#define DEVICE_CODE_URL "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode"
#define TOKEN_URL       "https://login.microsoftonline.com/consumers/oauth2/v2.0/token"
#define CLIENT_ID       "1f907974-e22b-4810-a9de-d9647380c97e"
#define SCOPE           "xboxlive.signin%20openid%20profile%20offline_access"

// the refresh token is a credential for the signed-in Microsoft account. it is kept in the clear:
// the console has no key store to protect it with, and a key sat beside the file protects nothing.
#define TOKEN_PATH CELL_STREAM_SETTINGS_DIR "/xbox-token.txt"

// Xbox Live's leg of the chain: a Microsoft sign-in becomes an Xbox user token, that becomes a pass
// for the streaming service, and that finally becomes the session token and the regional host.
#define XBOX_USER_URL   "https://user.auth.xboxlive.com/user/authenticate"
#define XBOX_XSTS_URL   "https://xsts.auth.xboxlive.com/xsts/authorize"
#define STREAM_LOGIN_URL "https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user"
#define STREAM_OFFERING "xgpuweb"

// the streaming server asks for one more token when a session is ready, from the older Live endpoint
// rather than the sign-in one, and under a scope of its own
#define LIVE_TOKEN_URL  "https://login.live.com/oauth20_token.srf"
#define PASSPORT_SCOPE  "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN"

#define DEVICE_CODE_MAX 2048   // the codes this endpoint returns run close to 1 KB; twice that is headroom
#define TOKEN_MAX       8192   // the Xbox Live tokens are signed blobs of several KB, unlike the Microsoft ones
#define HOST_MAX        128
#define RESPONSE_MAX    16384
#define BODY_MAX        12288  // one body carries a whole Xbox token

static XcloudAuthState authState = XCLOUD_AUTH_IDLE;
static char userCode[XCLOUD_USER_CODE_MAX];
static char deviceCode[DEVICE_CODE_MAX];
static char refreshToken[TOKEN_MAX];
static char accessToken[TOKEN_MAX];
static char streamToken[TOKEN_MAX];
static char streamHost[HOST_MAX];
static char authError[128];

// the requests run one at a time, so they share these rather than putting several KB on the stack
static char requestBody[BODY_MAX];
static char response[RESPONSE_MAX];
static char xboxUserToken[TOKEN_MAX];        // who you are on Xbox Live
static char streamingAuthToken[TOKEN_MAX];   // permission to use the streaming service

static const HttpHeader FORM_HEADER = { "Content-Type", "application/x-www-form-urlencoded" };

static void failAuth(const char *reason)
{
   strCopy(authError, sizeof authError, reason);
   authState = XCLOUD_AUTH_FAILED;
   logError(TAG "sign-in failed: %s\n", reason);
}

// both posters put the answer in `response`; they return the HTTP status, or -1 when nothing came back

static int postForm(const char *url)
{
   int responseLength = 0, status = 0;
   if (fetchHttp("POST", url, &FORM_HEADER, 1, requestBody, getStrLen(requestBody), response, sizeof response,
                 &responseLength, &status) != 0)
      return -1;
   return status;
}

// the Xbox endpoints each want one header of their own alongside the content type
static int postJson(const char *url, const char *headerName, const char *headerValue)
{
   const HttpHeader headers[] = { { "Content-Type", "application/json" }, { headerName, headerValue } };
   int responseLength = 0, status = 0;
   if (fetchHttp("POST", url, headers, 2, requestBody, getStrLen(requestBody), response, sizeof response,
                 &responseLength, &status) != 0)
      return -1;
   return status;
}

static int saveRefreshToken(void)
{
   makeDirPath(CELL_STREAM_SETTINGS_DIR);
   return writeFile(TOKEN_PATH, refreshToken, getStrLen(refreshToken));
}

// swaps a refresh token for a fresh one, which is how a saved sign-in is checked for still being good.
// the access token that comes with it is what Xbox Live wants, so keep both.
static int redeemRefreshToken(void)
{
   snprintf(requestBody, sizeof requestBody, "client_id=%s&scope=%s&grant_type=refresh_token&refresh_token=%s",
            CLIENT_ID, SCOPE, refreshToken);

   if (postForm(TOKEN_URL) != 200) return -1;

   int responseLength = getStrLen(response);
   if (getJsonText(response, responseLength, "access_token", accessToken, sizeof accessToken) != 0) return -1;
   return getJsonText(response, responseLength, "refresh_token", refreshToken, sizeof refreshToken);
}

int startXcloudSignIn(void)
{
   authState = XCLOUD_AUTH_IDLE;
   userCode[0] = 0;
   authError[0] = 0;

   // a saved sign-in, if there is one. it only counts once the token has been redeemed: it expires,
   // and the user may have revoked it from their account since.
   if (readFile(TOKEN_PATH, refreshToken, sizeof refreshToken) > 0 && redeemRefreshToken() == 0) {
      saveRefreshToken();
      authState = XCLOUD_AUTH_SIGNED_IN;
      logInfo(TAG "signed in from the saved token\n");
      return 0;
   }
   refreshToken[0] = 0;

   // no saved sign-in: ask for a code the user can type in
   snprintf(requestBody, sizeof requestBody, "client_id=%s&scope=%s", CLIENT_ID, SCOPE);

   int status = postForm(DEVICE_CODE_URL);
   if (status != 200) {
      snprintf(authError, sizeof authError, "could not reach Microsoft (%d)", status);
      authState = XCLOUD_AUTH_FAILED;
      logError(TAG "device code request failed, status=%d\n", status);
      return -1;
   }

   int responseLength = getStrLen(response);
   if (getJsonText(response, responseLength, "user_code", userCode, sizeof userCode) != 0 ||
       getJsonText(response, responseLength, "device_code", deviceCode, sizeof deviceCode) != 0) {
      failAuth("Microsoft's answer made no sense");
      return -1;
   }

   authState = XCLOUD_AUTH_WAITING_FOR_USER;
   logInfo(TAG "sign-in code %s - enter it at microsoft.com/link\n", userCode);
   return 0;
}

void pollXcloudSignIn(void)
{
   if (authState != XCLOUD_AUTH_WAITING_FOR_USER) return;

   snprintf(requestBody, sizeof requestBody,
            "client_id=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=%s", CLIENT_ID,
            deviceCode);

   int status = postForm(TOKEN_URL);
   if (status < 0) return;   // a dropped request is not a refusal; the next poll tries again

   int responseLength = getStrLen(response);

   if (status == 200) {
      if (getJsonText(response, responseLength, "refresh_token", refreshToken, sizeof refreshToken) != 0 ||
          getJsonText(response, responseLength, "access_token", accessToken, sizeof accessToken) != 0) {
         failAuth("signed in but no token came back");
         return;
      }
      saveRefreshToken();
      authState = XCLOUD_AUTH_SIGNED_IN;
      logInfo(TAG "signed in\n");
      return;
   }

   // "authorization_pending" is the normal answer until the user finishes; anything else ends it
   char error[64];
   if (getJsonText(response, responseLength, "error", error, sizeof error) != 0) return;
   if (strEq(error, "authorization_pending") || strEq(error, "slow_down")) return;

   if (strEq(error, "expired_token")) failAuth("the code expired, try again");
   else failAuth(error);
}

// one leg of the Xbox Live chain: post the body already in requestBody, then lift `key` out of the
// answer into `out`. every leg fails the same way, so they all report it the same way.
static int exchangeToken(const char *url, const char *headerName, const char *headerValue, const char *key, char *out,
                         int capacity, const char *legName)
{
   int status = postJson(url, headerName, headerValue);
   if (status != 200) {
      snprintf(authError, sizeof authError, "Xbox Live refused the %s step (%d)", legName, status);
      logError(TAG "%s failed, status=%d\n", legName, status);
      return -1;
   }
   if (getJsonText(response, getStrLen(response), key, out, capacity) != 0) {
      snprintf(authError, sizeof authError, "no %s came back from the %s step", key, legName);
      logError(TAG "%s gave no %s\n", legName, key);
      return -1;
   }
   return 0;
}

// the default region's base address, out of the regions list the login answer carries
static int readDefaultRegion(void)
{
   int settingsStart = 0, settingsEnd = 0;
   if (getJsonObject(response, getStrLen(response), "offeringSettings", &settingsStart, &settingsEnd) != 0) return -1;

   int listStart = 0, listEnd = 0;
   if (findJsonArray(response + settingsStart, settingsEnd - settingsStart, "regions", &listStart, &listEnd) != 0)
      return -1;

   const char *list = response + settingsStart;
   int regionStart = 0, regionEnd = 0, offset = listStart;
   char isDefault[8];
   while ((offset = readJsonObject(list, listEnd, offset, &regionStart, &regionEnd)) != 0) {
      int regionLength = regionEnd - regionStart;
      if (getJsonText(list + regionStart, regionLength, "isDefault", isDefault, sizeof isDefault) == 0 &&
          strEq(isDefault, "true"))
         return getJsonText(list + regionStart, regionLength, "baseUri", streamHost, sizeof streamHost);
   }
   return -1;
}

int fetchXcloudStreamingToken(void)
{
   if (authState != XCLOUD_AUTH_SIGNED_IN) return -1;

   // the Microsoft sign-in becomes an Xbox Live user token
   snprintf(requestBody, sizeof requestBody,
            "{\"Properties\":{\"AuthMethod\":\"RPS\",\"RpsTicket\":\"d=%s\",\"SiteName\":\"user.auth.xboxlive.com\"},"
            "\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}",
            accessToken);
   if (exchangeToken(XBOX_USER_URL, "x-xbl-contract-version", "1", "Token", xboxUserToken, sizeof xboxUserToken,
                     "Xbox user token") != 0)
      return -1;

   // and that becomes a pass for the streaming service in particular
   snprintf(requestBody, sizeof requestBody,
            "{\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"%s\"]},"
            "\"RelyingParty\":\"http://gssv.xboxlive.com/\",\"TokenType\":\"JWT\"}",
            xboxUserToken);
   if (exchangeToken(XBOX_XSTS_URL, "x-xbl-contract-version", "1", "Token", streamingAuthToken,
                     sizeof streamingAuthToken, "streaming authorization") != 0)
      return -1;

   // which the streaming service trades for the session token, and tells us which region to use
   snprintf(requestBody, sizeof requestBody, "{\"token\":\"%s\",\"offeringId\":\"%s\"}", streamingAuthToken,
            STREAM_OFFERING);
   if (exchangeToken(STREAM_LOGIN_URL, "x-gssv-client", "XboxComBrowser", "gsToken", streamToken, sizeof streamToken,
                     "streaming sign-in") != 0)
      return -1;

   if (readDefaultRegion() != 0) {
      strCopy(authError, sizeof authError, "no region came back to stream from");
      logError(TAG "streaming sign-in named no default region\n");
      return -1;
   }

   logInfo(TAG "streaming region %s (token %d bytes)\n", streamHost, getStrLen(streamToken));
   return 0;
}

int fetchXcloudPassportToken(char *out, int capacity)
{
   if (authState != XCLOUD_AUTH_SIGNED_IN) return -1;

   snprintf(requestBody, sizeof requestBody,
            "client_id=%s&scope=%s&grant_type=refresh_token&refresh_token=%s", CLIENT_ID, PASSPORT_SCOPE,
            refreshToken);

   if (postForm(LIVE_TOKEN_URL) != 200) {
      logError(TAG "passport token request was refused\n");
      return -1;
   }
   return getJsonText(response, getStrLen(response), "access_token", out, capacity);
}

XcloudAuthState getXcloudAuthState(void) { return authState; }
const char *getXcloudStreamHost(void) { return streamHost; }
const char *getXcloudStreamToken(void) { return streamToken; }
const char *getXcloudUserCode(void) { return userCode; }
const char *getXcloudAuthError(void) { return authError; }
