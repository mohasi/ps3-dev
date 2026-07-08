// transport-cellhttp - the system-TLS (cellHttp) transport for the http module (see http-transport.h).
//
// The default transport: it reaches any host the console's firmware TLS can (RSA and ECDHE-RSA cipher
// suites), which covers Google/YouTube/googlevideo, and is free weight for PRXs (the stubs are firmware-
// resident). Hosts that serve ECDSA-only certs (Cloudflare) need the modern BearSSL transport instead
// (simple-lib-https), which overrides this one when an app opts into it.
//
// One request = one transaction on its OWN client. Two transactions draining two response bodies in
// interleaved partial chunks on ONE shared client corrupts that client's receive state and hard-faults
// (the SDK drains one body at a time per client). So a request keeps a client to itself for its whole life.
//
// Reuse: cellHttp keeps a request's persistent socket INSIDE its client (keep-alive), so we recycle the
// finished client - with its warm socket - through a pool instead of destroying it. The pool is keyless:
// any idle client serves any host (libhttp reuses its warm socket if it has one to that host, else opens a
// fresh one). Because a checkout removes the client from the pool, no two live transactions ever share one.
//
// netLock serialises connection setup/teardown and the pool (create/destroy/sendRequest and the pool
// array). Body reads run UNLOCKED - each transaction is only ever touched by its own reader, which is
// libhttp's supported model, and locking reads starved playback (a throttled stream holds a read open for
// tens of seconds while the other got nothing).

#include "http-transport.h"
#include "cellhttp-stack.h"     // ensureHttpStack, verifyCellHttpsCert
#include "thread.h"             // lock helpers
#include "dbg.h"

#include <stdlib.h>
#include <string.h>

#include <cell/http.h>

// bound how long a single read can sit with NO data arriving, so a dead connection fails instead of
// hanging forever. Does not bound a slow-but-alive (trickling) read: the timer resets whenever bytes
// arrive, and the call returns only when the buffer is full or the body ends.
#define RECV_TIMEOUT_US    (1000 * 1000)
// bound the TCP connect + TLS handshake + send. Without it a bad connection hangs ~30s on the OS default.
#define CONNECT_TIMEOUT_US (10 * 1000 * 1000)
#define OPEN_ATTEMPTS      3   // open tries (the first HTTPS to a fresh host can blip: rc=0x80710d23)
#define CLIENT_POOL_SIZE   8   // idle keep-alive clients kept for reuse (covers the concurrent workers)

// one request: a client borrowed from the pool for its lifetime, plus this request's own transaction.
typedef struct {
   CellHttpClientId client;
   CellHttpTransId  trans;
   void            *uriPool;
   int              drained;   // body read to a clean end -> the socket is synced and safe to reuse
} CellConn;

static CellHttpClientId clientPool[CLIENT_POOL_SIZE];
static sys_lwmutex_t    netLock;
static int              locksReady;
static void lockNet(void)   { if (locksReady) lock(&netLock); }
static void unlockNet(void) { if (locksReady) unlock(&netLock); }

// section: client pool (callers hold netLock)

// take an idle client out of the pool, or 0 if none is parked.
static CellHttpClientId checkoutClient(void)
{
   for (int i = 0; i < CLIENT_POOL_SIZE; i++)
      if (clientPool[i]) { CellHttpClientId client = clientPool[i]; clientPool[i] = 0; return client; }
   return 0;
}

// park a client for reuse; destroy it if the pool is full.
static void checkinClient(CellHttpClientId client)
{
   for (int i = 0; i < CLIENT_POOL_SIZE; i++)
      if (!clientPool[i]) { clientPool[i] = client; return; }
   cellHttpDestroyClient(client);
}

// create a client with our tls verify callback and keep-alive on. the stack must be up (ensureHttpStack).
static int createClient(CellHttpClientId *client)
{
   if (cellHttpCreateClient(client) < 0) { *client = 0; return -1; }
   cellHttpClientSetSslCallback(*client, verifyCellHttpsCert, NULL);
   cellHttpClientSetConnTimeout(*client, CONNECT_TIMEOUT_US);   // fail a bad connect fast instead of ~30s
   cellHttpClientSetSendTimeout(*client, CONNECT_TIMEOUT_US);
   cellHttpClientSetRecvTimeout(*client, RECV_TIMEOUT_US);      // don't block on a stalled stream
   cellHttpClientSetKeepAlive(*client, true);                   // hold the socket open for the next request
   cellHttpClientSetPerHostKeepAliveMax(*client, 1);            // one persistent connection kept per host
   return 0;
}

// destroy this request's transaction and uri pool; the client is recycled separately.
static void endTransaction(CellConn *conn)
{
   if (conn->trans)   { cellHttpDestroyTransaction(conn->trans); conn->trans = 0; }
   if (conn->uriPool) { free(conn->uriPool); conn->uriPool = NULL; }
}

// map a method string to libhttp's method token (CELL_HTTP_METHOD_* are const char *, not ints).
static const char *toCellMethod(const char *method)
{
   if (strcmp(method, "POST") == 0) return CELL_HTTP_METHOD_POST;
   return CELL_HTTP_METHOD_GET;
}

// build the transaction on conn, add headers + optional body, send it, and read the status. caller holds
// netLock. reports *status (any code) and, when asked, *totalSize (Content-Length). 0 / -1 (no response).
static int sendCellRequest(CellConn *conn, const char *method, const char *url, const HttpHeader *headers,
                           int headerCount, const void *body, int bodyLen, int *status, uint64_t *totalSize)
{
   size_t poolSize = 0;
   if (cellHttpUtilParseUri(NULL, url, NULL, 0, &poolSize) < 0) { logError("[http] parseUri(size) failed\n"); return -1; }
   conn->uriPool = malloc(poolSize);
   if (!conn->uriPool) return -1;
   CellHttpUri uri;
   if (cellHttpUtilParseUri(&uri, url, conn->uriPool, poolSize, NULL) < 0) { logError("[http] parseUri failed\n"); return -1; }

   if (cellHttpCreateTransaction(&conn->trans, conn->client, toCellMethod(method), &uri) < 0) { conn->trans = 0; logError("[http] createTransaction failed\n"); return -1; }

   for (int i = 0; i < headerCount; i++) {
      CellHttpHeader header = { headers[i].name, headers[i].value };
      cellHttpRequestAddHeader(conn->trans, &header);
   }
   if (body && bodyLen > 0) cellHttpRequestSetContentLength(conn->trans, (uint64_t)bodyLen);

   size_t sent = 0;
   int rc = cellHttpSendRequest(conn->trans, body, body ? (size_t)bodyLen : 0, &sent);   // connect + TLS + send
   if (rc < 0) { logError("[http] sendRequest failed, rc=0x%x\n", rc); return -1; }

   int code = 0;
   if ((rc = cellHttpResponseGetStatusCode(conn->trans, &code)) < 0) { logError("[http] getStatusCode failed, rc=0x%x\n", rc); return -1; }
   if (status) *status = code;
   if (totalSize) { uint64_t length = 0; *totalSize = cellHttpResponseGetContentLength(conn->trans, &length) == 0 ? length : 0; }
   return 0;
}

// section: HttpTransport

static void *openCell(const char *method, const char *url, const HttpHeader *headers, int headerCount,
                      const void *body, int bodyLen, int *status, uint64_t *totalSize)
{
   if (status) *status = 0;
   if (totalSize) *totalSize = 0;
   if (ensureHttpStack() != 0) return NULL;

   CellConn *conn = calloc(1, sizeof *conn);
   if (!conn) return NULL;

   // attempt 0 reuses a warm pooled client; a stale reuse (or the fresh-host sendRequest blip, rc=0x80710d23)
   // drops the client and retries fresh. one loop covers both retry reasons.
   int ok = 0;
   for (int attempt = 0; attempt < OPEN_ATTEMPTS && !ok; attempt++) {
      lockNet();
      if (attempt > 0) { endTransaction(conn); if (conn->client) { cellHttpDestroyClient(conn->client); conn->client = 0; } }
      conn->client = attempt == 0 ? checkoutClient() : 0;
      int reused = conn->client != 0;
      if (!conn->client && createClient(&conn->client) != 0) { unlockNet(); break; }
      ok = sendCellRequest(conn, method, url, headers, headerCount, body, bodyLen, status, totalSize) == 0;
      unlockNet();
      // a stale reuse retries fresh with no wait; a fresh-host blip waits before retrying.
      if (!ok && !reused && attempt + 1 < OPEN_ATTEMPTS) { logWarn("[http] open failed, retry %d/%d\n", attempt + 1, OPEN_ATTEMPTS - 1); sleepMs(100); }
   }
   if (!ok) { lockNet(); endTransaction(conn); if (conn->client) cellHttpDestroyClient(conn->client); unlockNet(); free(conn); return NULL; }
   return conn;
}

static int64_t readCell(void *handle, void *buffer, uint64_t cap)
{
   CellConn *conn = handle;
   size_t got = 0;
   int rc = cellHttpRecvResponse(conn->trans, buffer, (size_t)cap, &got);
   if (got > 0) return (int64_t)got;                              // commit whatever arrived
   if (rc == (int)CELL_HTTP_ERROR_NET_SELECT_TIMEOUT) return -2;  // stalled this turn, connection alive
   if (rc < 0) return -1;                                         // connection died
   conn->drained = 1;                                             // clean end of body: socket synced for reuse
   return 0;
}

static void closeCell(void *handle)
{
   CellConn *conn = handle;
   if (!conn) return;
   lockNet();
   CellHttpClientId client = conn->client;
   int drained = conn->drained;
   endTransaction(conn);
   if (client) {
      // a body abandoned mid-read leaves a desynced socket - drop it before the client goes back for reuse.
      if (!drained) cellHttpClientCloseAllConnections(client);
      checkinClient(client);
   }
   unlockNet();
   free(conn);
}

static void shutdownCell(void)
{
   if (!locksReady) return;
   lockNet();
   for (int i = 0; i < CLIENT_POOL_SIZE; i++)
      if (clientPool[i]) { cellHttpDestroyClient(clientPool[i]); clientPool[i] = 0; }
   unlockNet();
}

static const HttpTransport CELLHTTP_TRANSPORT = { openCell, readCell, closeCell, shutdownCell };

void initSystemHttp(void)
{
   if (!locksReady) { createLock(&netLock); locksReady = 1; }
   bindHttpTransport(&CELLHTTP_TRANSPORT, 0);
}
