// transport-bearssl - the modern-TLS (BearSSL) transport for the http module (see http-transport.h).
//
// The opt-in override: an app that calls initModernHttp() routes EVERY http request and stream through
// BearSSL instead of cellHttp, so it reaches hosts the firmware's RSA-only TLS can't (ECDSA certs) at the
// cost of ~80 KB.
//
// Streaming (a GET carrying a Range header) uses HTTP/1.1 and always opens a fresh connection; one-shot
// requests use HTTP/1.0 keep-alive and reuse a pooled connection to the same host, so a screen full of
// thumbnails costs a handful of TLS handshakes instead of one per image. HTTP/1.0 keeps the response
// un-chunked; the hosts we talk to answer a Range request with 206 + Content-Length, so no chunked
// decoding is needed either way.

#include "http-transport.h"
#include "tls-transport.h"
#include "thread.h"   // pool lock
#include "dbg.h"

#include <stdio.h>    // snprintf
#include <string.h>   // strlen, strcpy, strncmp, strcmp

#define MAX_REDIRECTS 5   // googlevideo 302s fresh connections to rebalance across its rrN servers
#define URL_MAX       2048
#define POOL_SIZE     8   // idle keep-alive connections kept for reuse (covers the concurrent thumbnail workers)

// section: connection pool - idle keep-alive connections held for reuse, keyed by host. one-shot requests
// (thumbnails, api) hand a connection back here on close instead of tearing down its TLS handshake, and
// the next request to the same host picks it up. thread-safe: thumbnails fetch on several workers at once.
static TlsConn      *pool[POOL_SIZE];
static sys_lwmutex_t poolLock;
static int           poolReady;

// take an idle connection to `host` out of the pool, or NULL if none is parked.
static TlsConn *checkoutConn(const char *host)
{
   if (!poolReady) return NULL;
   TlsConn *found = NULL;
   lock(&poolLock);
   for (int i = 0; i < POOL_SIZE; i++)
      if (pool[i] && strcmp(getTlsHost(pool[i]), host) == 0) { found = pool[i]; pool[i] = NULL; break; }
   unlock(&poolLock);
   return found;
}

// park an idle, reusable connection for the next request; close it if the pool is full.
static void checkinConn(TlsConn *conn)
{
   int parked = 0;
   if (poolReady) {
      lock(&poolLock);
      for (int i = 0; i < POOL_SIZE; i++)
         if (!pool[i]) { pool[i] = conn; parked = 1; break; }
      unlock(&poolLock);
   }
   if (!parked) closeTlsConn(conn);
}

// case-insensitive test for the "Range" header, used to pick the HTTP version (streaming vs one-shot).
static int isRangeHeader(const char *name)
{
   const char *want = "range";
   for (int i = 0; i < 5; i++) {
      char c = name[i];
      if (c >= 'A' && c <= 'Z') c += 32;
      if (c != want[i]) return 0;
   }
   return name[5] == '\0';
}

static int isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

// get a connection to `host` (reused from the pool if possible), send the request, read the response head.
// a pooled connection the server closed while idle fails the first write - so retry once with a fresh one.
// returns the live connection positioned at the body, or NULL.
static TlsConn *sendReusable(const char *host, const char *path, const char *method, const char *version,
                            int keepAlive, const char *headerBlock, const void *body, int bodyLen,
                            int *status, uint64_t *total)
{
   for (int attempt = 0; attempt < 2; attempt++) {
      TlsConn *conn = (keepAlive && attempt == 0) ? checkoutConn(host) : NULL;   // retry always opens fresh
      int reused = conn != NULL;
      if (!reused) conn = openTlsConn(host);
      if (!conn) return NULL;

      if (sendTlsRequest(conn, method, version, keepAlive, host, path, headerBlock, body, bodyLen) == 0 &&
          readTlsHead(conn, status, total) == 0)
         return conn;

      if (!reused) { logError("[tls] %s: request failed (bearssl err=%d)\n", host, getLastTlsError(conn)); closeTlsConn(conn); return NULL; }
      closeTlsConn(conn);
      logWarn("[tls] reused %s went stale, retrying fresh\n", host);   // loop retries with a fresh connection
   }
   return NULL;
}

static void *openBearssl(const char *method, const char *url, const HttpHeader *headers, int headerCount,
                         const void *body, int bodyLen, int *status, uint64_t *totalSize)
{
   if (status) *status = 0;
   if (totalSize) *totalSize = 0;

   // format the caller's headers into one block, and note if a Range is present (=> streaming => 1.1). the
   // block (Range + UA) is reused unchanged across redirects so the same byte range is requested from the
   // server we get redirected to.
   char headerBlock[2048];
   int length = 0, hasRange = 0;
   for (int i = 0; i < headerCount; i++) {
      int written = snprintf(headerBlock + length, sizeof headerBlock - length, "%s: %s\r\n", headers[i].name, headers[i].value);
      if (written < 0 || length + written >= (int)sizeof headerBlock) { logError("[tls] request headers too long\n"); return NULL; }
      length += written;
      if (isRangeHeader(headers[i].name)) hasRange = 1;
   }
   headerBlock[length] = '\0';

   char target[URL_MAX];
   if (strlen(url) >= sizeof target) { logError("[tls] url too long\n"); return NULL; }
   strcpy(target, url);

   // follow redirects: googlevideo 302s a fresh connection to another rrN host. after the first hop we
   // re-request as a bodyless GET (303 semantics), which is what our redirects always are (media GETs).
   for (int hop = 0; ; hop++) {
      char host[256];
      const char *path;
      if (splitHttpsUrl(target, host, sizeof host, &path) != 0) return NULL;

      int code = 0;
      uint64_t total = 0;
      // one-shot requests (no Range) ask for keep-alive and reuse a pooled connection; streaming opens fresh.
      TlsConn *conn = sendReusable(host, path, hop ? "GET" : method, hasRange ? "HTTP/1.1" : "HTTP/1.0",
                                   !hasRange, headerBlock, hop ? NULL : body, hop ? 0 : bodyLen, &code, &total);
      if (!conn) return NULL;

      char location[URL_MAX];
      if (isRedirect(code) && hop < MAX_REDIRECTS &&
          getTlsHeader(conn, "location", location, sizeof location) == 0 &&
          strncmp(location, "https://", 8) == 0 && strlen(location) < sizeof target) {
         closeTlsConn(conn);
         strcpy(target, location);
         continue;
      }

      if (status) *status = code;
      if (totalSize) *totalSize = total;
      return conn;
   }
}

static int64_t readBearssl(void *handle, void *buffer, uint64_t cap)
{
   return recvTls((TlsConn *)handle, buffer, cap);   // >0 got, 0 eof, -1 dead (http reconnects on stream)
}

static void closeBearssl(void *handle)
{
   TlsConn *conn = handle;
   if (isTlsReusable(conn)) checkinConn(conn);   // park it for the next request to this host
   else closeTlsConn(conn);
}

// close every idle pooled connection; called at app shutdown via shutdownHttp().
static void shutdownBearssl(void)
{
   if (!poolReady) return;
   lock(&poolLock);
   for (int i = 0; i < POOL_SIZE; i++)
      if (pool[i]) { closeTlsConn(pool[i]); pool[i] = NULL; }
   unlock(&poolLock);
}

static const HttpTransport BEARSSL_TRANSPORT = { openBearssl, readBearssl, closeBearssl, shutdownBearssl };

void initModernHttp(void)
{
   if (!poolReady) { createLock(&poolLock); poolReady = 1; }
   initTlsResolver();   // serialise gethostbyname before any worker thread opens a connection
   bindHttpTransport(&BEARSSL_TRANSPORT, 1);   // override: modern TLS wins over the system backend
}
