// http-fetch - blocking HTTPS helpers (see http-fetch.h).
//
// Per request: create a client + transaction, send, read the response; destroy them at the end. The
// libhttp/libssl/libhttps stack itself is brought up once and kept resident (ensureHttpStack in
// simple-lib-core, shared with the http-fs streaming backend) - cycling init/end faults after a stream
// is torn down mid-download. Generalised to any method + headers + body; the body drains into a caller
// buffer (httpFetch).

#include "http-fetch.h"
#include "http-fs.h"           // ensureHttpStack - the shared, resident http/ssl stack
#include "dbg.h"               // logWarn on truncated responses

#include <stdlib.h>
#include <string.h>

#include <cell/ssl.h>

#define HTTP_TIMEOUT_US (10 * 1000 * 1000)   // per-op network timeout; a stall fails instead of hanging

// enforce verification: youtube/googlevideo chain to CAs already in the store.
static int32_t sslVerifyCb(uint32_t verifyErr, CellSslCert const cert[], int certNum, const char *hostname, CellHttpSslId id, void *arg)
{
   (void)cert; (void)certNum; (void)hostname; (void)id; (void)arg;
   return verifyErr;   // non-zero fails the handshake
}

// live resources for one request; httpEnd releases them (the shared stack stays up).
typedef struct {
   void *uriPool;
   CellHttpClientId client;
   CellHttpTransId trans;
} HttpCall;

// bring up http/ssl, create the client + transaction, send the request (and body),
// and read the status code. 0 on success (status in *statusOut), negative otherwise.
// The caller must httpEnd() the call on every outcome.
static int httpBegin(HttpCall *call, const char *method, const char *url,
                     const CellHttpHeader *headers, int headerCount,
                     const void *body, int bodyLen, int *statusOut)
{
   int ret = -1;

   // the http/ssl/https stack is brought up once and kept resident (shared with http-fs)
   if (ensureHttpStack() != 0) return -1;

   if ((ret = cellHttpCreateClient(&call->client)) < 0) { call->client = 0; return ret; }
   cellHttpClientSetSslCallback(call->client, sslVerifyCb, NULL);

   // bound every network op so a stalled connection fails cleanly instead of hanging the app forever
   cellHttpClientSetConnTimeout(call->client, HTTP_TIMEOUT_US);
   cellHttpClientSetSendTimeout(call->client, HTTP_TIMEOUT_US);
   cellHttpClientSetRecvTimeout(call->client, HTTP_TIMEOUT_US);

   // no keep-alive: a transaction destroyed with body bytes still unread would otherwise be PARKED in
   // libhttp's pool desynchronized, and the NEXT request faults inside libhttp (crashed the app whenever
   // a response outgrew the caller's buffer). Off means destroy closes the socket. Same rationale as
   // http-fs's streaming clients.
   cellHttpClientSetKeepAlive(call->client, false);

   // parse uri (size then fill)
   size_t poolSize = 0;
   if ((ret = cellHttpUtilParseUri(NULL, url, NULL, 0, &poolSize)) < 0) return ret;
   call->uriPool = malloc(poolSize);
   if (!call->uriPool) return -1;
   CellHttpUri uri;
   if ((ret = cellHttpUtilParseUri(&uri, url, call->uriPool, poolSize, NULL)) < 0) return ret;

   if ((ret = cellHttpCreateTransaction(&call->trans, call->client, method, &uri)) < 0) { call->trans = 0; return ret; }
   for (int i = 0; i < headerCount; i++)
      cellHttpRequestAddHeader(call->trans, &headers[i]);
   if (body && bodyLen > 0)
      cellHttpRequestSetContentLength(call->trans, (uint64_t)bodyLen);

   // send request (+ body); the TLS handshake happens here
   size_t sent = 0;
   if ((ret = cellHttpSendRequest(call->trans, body, body ? (size_t)bodyLen : 0, &sent)) < 0) return ret;

   int code = 0;
   if ((ret = cellHttpResponseGetStatusCode(call->trans, &code)) < 0) return ret;
   if (statusOut) *statusOut = code;
   return 0;
}

static void httpEnd(HttpCall *call)
{
   if (call->trans) cellHttpDestroyTransaction(call->trans);
   if (call->client) cellHttpDestroyClient(call->client);
   if (call->uriPool) free(call->uriPool);
   // the shared http/ssl stack stays resident (ensureHttpStack); only this request's client is freed
}

int httpFetch(const char *method, const char *url,
              const CellHttpHeader *headers, int headerCount,
              const void *body, int bodyLen,
              char *out, int outCap, int *outLen, int *statusOut)
{
   HttpCall call;
   memset(&call, 0, sizeof call);
   if (outLen) *outLen = 0;
   if (statusOut) *statusOut = 0;

   int ret = httpBegin(&call, method, url, headers, headerCount, body, bodyLen, statusOut);
   if (ret < 0) { httpEnd(&call); return ret; }

   // drain the body into out
   int total = 0;
   while (total < outCap - 1) {
      size_t got = 0;
      ret = cellHttpRecvResponse(call.trans, out + total, (size_t)(outCap - 1 - total), &got);
      if (ret < 0) { httpEnd(&call); return ret; }
      if (got == 0) break;
      total += (int)got;
   }
   out[total] = '\0';

   // if the response outgrew the buffer, discard the rest so the connection ends at the body boundary
   // (never destroy a transaction mid-body); the caller still gets the truncated text
   if (total >= outCap - 1) {
      logWarn("[http] response truncated at %d bytes, draining the rest\n", total);
      char discard[2048];
      for (;;) {
         size_t got = 0;
         if (cellHttpRecvResponse(call.trans, discard, sizeof discard, &got) < 0 || got == 0) break;
      }
   }

   if (outLen) *outLen = total;
   httpEnd(&call);
   return 0;
}
