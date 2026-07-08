// http-fs - read-only VFS backend that streams http(s):// urls (see http-fs.h).
//
// Each open stream owns a ring buffer and a background prefetch thread that keeps
// the ring filled ahead of the reader. The reader serves from the ring and never
// touches the network, so a network dip drains the buffer instead of stalling
// playback. A single seek (position moved off the buffered window) makes the
// prefetch reconnect with a fresh Range request.
//
// Each stream owns its OWN cellHttp client. Two streams reading two response bodies in interleaved
// partial chunks on ONE shared client corrupts that client's receive state and hard-faults (the SDK
// only ever drains one body at a time per client); a client per stream keeps their receive state
// separate. libhttp/libssl themselves (and their process-wide pools) stay shared and refcounted.
//
// Two locks, never held at the same time (so they can't deadlock):
//  - netLock: serialises connection setup/teardown (create/destroy/sendRequest allocate and free from
//    libhttp's process-wide pools). Body recvs run UNLOCKED - each transaction is only ever touched by
//    its own thread, which is libhttp's supported threading model. Locking recvs starved playback: a
//    throttled stream (googlevideo trickles long media at ~media bitrate) holds a 256 KB recv open for
//    tens of seconds, and the other stream's prefetch got nothing.
//  - each stream's stateLock: guards its ring window + read position.
//
// libhttp/libssl are brought up on the first open and torn down on the last close (refcounted). They
// must not run at the same time as another per-call http user in the process; here the api resolve
// finishes before playback opens a stream.

#include "http-fs.h"
#include "vfs.h"
#include "vfs-internal.h"        // setUrlVfsBackend
#include "thread.h"              // lock + thread helpers
#include "string-utilities.h"    // strCopy
#include "dbg.h"                 // logInfo/logError

#include <stdlib.h>
#include <string.h>

#include <cell/http.h>
#include <cell/ssl.h>
#include <sys/sys_time.h>   // TEMP (step 1): microsecond timing of connection setup vs server response

// pools are process-wide (carved by cellHttpInit/cellSslInit), not per-client. every simultaneous HTTPS
// connection draws its buffers + TLS record/handshake state from here at once. Sony's single-HTTPS sample
// budgets 64 KB HTTP / 256 KB SSL for ONE connection. peak concurrency while browsing is the thumbnail
// fetch pool (THUMB_THREADS, 8); playback uses just 2 (video + audio) and never overlaps it (the search
// worker stops before playback). this is an EBOOT app with hundreds of MB free, so sized well past 8
// connections - RAM is cheap here and undersizing corrupts a body or faults inside libhttp/libssl.
#define HTTP_POOL_SIZE (1024 * 1024)   // ~16 connections' worth of HTTP buffers
#define SSL_POOL_SIZE  (3 * 1024 * 1024)   // ~12 connections' worth of TLS state
#define HTTP_URL_MAX   2048
#define MAX_STREAMS    4

// bound how long a single recv can sit with NO data arriving, so a dead connection fails instead of
// hanging the prefetch forever. Note this does not bound a slow-but-alive (trickling) recv: the timer
// resets whenever bytes arrive, and the call returns only when the buffer is full or the body ends.
#define RECV_TIMEOUT_US (1000 * 1000)

// bound the TCP connect + TLS handshake + send. Without it a bad connection hangs ~30s on the OS default
// (seen as rc=0x80710102 after a long "Opening..."); 10s fails fast so the open retry can try again.
#define CONNECT_TIMEOUT_US (10 * 1000 * 1000)

// a widely-accepted UA; some CDNs 403 a request with none. generic on purpose -
// this backend is not youtube-specific.
#define HTTP_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

// the ring keeps a window of streamed bytes indexed by file offset % RING_SIZE.
// the prefetch thread fills it forward to PREFETCH_AHEAD past the read position;
// the remaining RING_SIZE-PREFETCH_AHEAD is history for small back-reads. A read
// position more than FORWARD_RECONNECT past the window is treated as a seek.
#define RING_SIZE         (4 * 1024 * 1024)
#define PREFETCH_AHEAD    (2 * 1024 * 1024)
// bytes per recv step. cellHttpRecvResponse returns only once THIS buffer fills (verified on hardware:
// got==chunk every recv), so on a throttled stream the chunk size bounds two things on a seek: how long
// the in-flight recv runs before the prefetch can react to a reconnect, and how long the first post-
// reconnect recv takes to deliver the reader's bytes. The audio stream trickles at ~30-40 KB/s, so 32 KB
// stalled a resume ~1 s per reconnect; 8 KB fills in ~200 ms, cutting reconnect reaction + first-byte
// delivery ~4x. Throughput is server-bound (recv returns then immediately re-recvs), so a smaller chunk
// costs only a few extra syscalls, not bandwidth.
#define PREFETCH_CHUNK    (8 * 1024)
#define FORWARD_RECONNECT (1 * 1024 * 1024)
#define PREFETCH_ERROR_RETRIES 3   // reconnects to try on a failed recv/open before latching an error
#define STREAM_OPEN_ATTEMPTS   3   // probe-open tries before giving up (first HTTPS to a host can blip)

typedef struct {
   int              inUse;
   char             url[HTTP_URL_MAX];
   uint8_t         *ring;
   uint64_t         position;      // reader's logical read position (guarded by stateLock)
   uint64_t         ringStart;     // lowest file offset still in the ring (prefetch advances it)
   uint64_t         ringEnd;       // one past the highest streamed byte
   uint64_t         size;          // total file size from Content-Length (0 if unknown)
   CellHttpClientId client;        // this stream's own http client (isolated receive state)
   CellHttpTransId  trans;         // 0 when none open
   void            *uriPool;
   int              needReconnect; // reader -> prefetch: position moved off the window, reopen there
   uint64_t         diagSeekReqUs; // TEMP: when a seek requested a reconnect, to measure prefetch reaction
   int              diagRecvBudget;// TEMP: log the first few recvs after a reconnect (fill-vs-available + reburst)
   int              errorRetries;  // consecutive failed recv/reconnect attempts (reset on progress)
   volatile int     running;       // prefetch thread runs while set
   volatile int     atEof;         // stream reached its end
   volatile int     error;         // a transaction/recv failed beyond retry
   sys_lwmutex_t    stateLock;     // guards ring window + position + flags
   sys_ppu_thread_t prefetchThread;
   int              prefetchActive;
} HttpStream;

static HttpStream    streams[MAX_STREAMS];
static sys_lwmutex_t netLock;       // serialises the cellHttp calls (they share libhttp's process-wide pools)
static int           locksReady;

// libhttp/libssl are brought up ONCE and kept resident for the whole app run (never torn down between
// plays). Cycling init->end->init faults inside libssl when a stream was torn down mid-download, so the
// stack stays up; http-fetch (the resolve path) shares it via ensureHttpStack too. Clients are per-stream.
static int    stackUp;
static void  *httpPool, *sslPool;

// a connection opened by statHttp on its own client (to read the size) and adopted by the openHttp
// that immediately follows, so stat+open cost one handshake and one client instead of two.
static struct { int valid; char url[HTTP_URL_MAX]; CellHttpClientId client; CellHttpTransId trans; void *uriPool; uint64_t size; } pendingOpen;

static void lockNet(void)   { if (locksReady) lock(&netLock); }
static void unlockNet(void) { if (locksReady) unlock(&netLock); }

static int closeHttp(VfsFile *file);

// section: libhttp/libssl bringup

// enforce verification: media hosts chain to CAs already in the console store.
static int32_t sslVerifyCb(uint32_t verifyErr, CellSslCert const cert[], int certNum, const char *hostname, CellHttpSslId id, void *arg)
{
   (void)cert; (void)certNum; (void)hostname; (void)id; (void)arg;
   return verifyErr;   // non-zero fails the handshake
}

static int loadSystemCerts(size_t *numOut, CellHttpsData **listOut)
{
   size_t size = 0;
   int ret = cellSslCertificateLoader(CELL_SSL_LOAD_CERT_ALL, NULL, 0, &size);
   if (ret < 0) return ret;
   char *buffer = malloc(size);
   if (!buffer) return -1;
   ret = cellSslCertificateLoader(CELL_SSL_LOAD_CERT_ALL, buffer, size, NULL);
   if (ret < 0) { free(buffer); return ret; }
   CellHttpsData *list = malloc(sizeof(CellHttpsData));
   if (!list) { free(buffer); return -1; }
   list[0].ptr = buffer;
   list[0].size = size;
   *listOut = list;
   *numOut = 1;
   return 0;
}

// brings libhttp/libssl/libhttps up once and leaves them resident. Idempotent; called from the app's
// single worker thread (resolve, then playback open) - never concurrently, so it needs no lock. 0 / -1.
int ensureHttpStack(void)
{
   if (stackUp) return 0;

   int httpOk = 0, sslOk = 0, httpsOk = 0;
   CellHttpsData *caList = NULL;
   size_t numCa = 0;
   int ret;

   httpPool = malloc(HTTP_POOL_SIZE);
   if (!httpPool || cellHttpInit(httpPool, HTTP_POOL_SIZE) < 0) goto fail;
   httpOk = 1;
   sslPool = malloc(SSL_POOL_SIZE);
   if (!sslPool || cellSslInit(sslPool, SSL_POOL_SIZE) < 0) goto fail;
   sslOk = 1;

   if (loadSystemCerts(&numCa, &caList) < 0) goto fail;
   ret = cellHttpsInit(numCa, caList);
   free(caList[0].ptr); free(caList);
   if (ret < 0) goto fail;
   httpsOk = 1;

   stackUp = 1;
   return 0;

fail:
   if (httpsOk) cellHttpsEnd();
   if (sslOk)   cellSslEnd();
   if (httpOk)  cellHttpEnd();
   free(sslPool);  sslPool = NULL;
   free(httpPool); httpPool = NULL;
   return -1;
}

// tears the stack down (app exit only; not called between plays).
void termHttpStack(void)
{
   if (!stackUp) return;
   cellHttpsEnd();
   cellSslEnd();
   cellHttpEnd();
   free(sslPool);  sslPool = NULL;
   free(httpPool); httpPool = NULL;
   stackUp = 0;
}

// section: transactions (all callers hold netLock)

// creates a per-stream client with our tls verify callback. the stack must be up (ensureHttpStack). 0 / -1.
static int createStreamClient(CellHttpClientId *client)
{
   if (cellHttpCreateClient(client) < 0) { *client = 0; return -1; }
   cellHttpClientSetSslCallback(*client, sslVerifyCb, NULL);
   cellHttpClientSetConnTimeout(*client, CONNECT_TIMEOUT_US);   // fail a bad connect fast instead of ~30s
   cellHttpClientSetSendTimeout(*client, CONNECT_TIMEOUT_US);
   cellHttpClientSetRecvTimeout(*client, RECV_TIMEOUT_US);   // don't block (holding netLock) on a stalled stream
   // we never reuse a connection, and a stream is often closed mid-download (user exits). With keep-alive
   // the mid-body socket would be PARKED in the pool desynchronized (per libhttp docs), which faults the
   // next request; off means destroy closes the socket instead of parking it.
   cellHttpClientSetKeepAlive(*client, false);
   return 0;
}

static void destroyTransaction(CellHttpTransId *trans, void **uriPool)
{
   if (*trans)   { cellHttpDestroyTransaction(*trans); *trans = 0; }
   if (*uriPool) { free(*uriPool); *uriPool = NULL; }
}

// tears down a transaction, its uri pool, and the client it ran on (a full stream/probe connection)
static void destroyConnection(CellHttpClientId *client, CellHttpTransId *trans, void **uriPool)
{
   destroyTransaction(trans, uriPool);
   if (*client) { cellHttpDestroyClient(*client); *client = 0; }
}

// writes "bytes=<offset>-" (open-ended range) into out.
static void formatRange(char *out, int cap, uint64_t offset)
{
   char digits[24];
   int digitCount = 0;
   if (offset == 0) digits[digitCount++] = '0';
   else for (; offset > 0; offset /= 10) digits[digitCount++] = (char)('0' + (int)(offset % 10));

   int length = 0;
   for (const char *prefix = "bytes="; *prefix && length < cap - 1; prefix++) out[length++] = *prefix;
   for (int i = 0; i < digitCount && length < cap - 1; i++) out[length++] = digits[digitCount - 1 - i];
   if (length < cap - 1) out[length++] = '-';
   out[length] = '\0';
}

// opens a GET that streams `url` from byte `offset` onward into *trans/*uriPool. optionally reports
// the total size (Content-Length of the range-0 request). caller holds netLock. 0 / -1.
static int openTransaction(CellHttpClientId client, const char *url, uint64_t offset, CellHttpTransId *trans, void **uriPool, uint64_t *sizeOut)
{
   destroyTransaction(trans, uriPool);

   size_t poolSize = 0;
   if (cellHttpUtilParseUri(NULL, url, NULL, 0, &poolSize) < 0) { logError("[http-fs] parseUri(size) failed\n"); return -1; }
   *uriPool = malloc(poolSize);
   if (!*uriPool) return -1;
   CellHttpUri uri;
   if (cellHttpUtilParseUri(&uri, url, *uriPool, poolSize, NULL) < 0) { logError("[http-fs] parseUri failed\n"); return -1; }

   if (cellHttpCreateTransaction(trans, client, CELL_HTTP_METHOD_GET, &uri) < 0) { *trans = 0; logError("[http-fs] createTransaction failed\n"); return -1; }

   char range[48];
   formatRange(range, sizeof range, offset);
   CellHttpHeader rangeHeader = { "Range", range };
   CellHttpHeader uaHeader    = { "User-Agent", HTTP_UA };
   cellHttpRequestAddHeader(*trans, &rangeHeader);
   cellHttpRequestAddHeader(*trans, &uaHeader);

   // TEMP (step 1): split the request into "connection setup" (cellHttpSendRequest does connect + TLS
   // handshake + send) vs "server first-response" (status code arrives). tells us where a reconnect's
   // time actually goes - keep-alive can only ever save the setup part.
   uint64_t tBeforeSend = sys_time_get_system_time();
   size_t sent = 0;
   int rc = cellHttpSendRequest(*trans, NULL, 0, &sent);
   uint64_t tAfterSend = sys_time_get_system_time();
   if (rc < 0) { logError("[http-fs] sendRequest failed, rc=0x%x\n", rc); return -1; }
   int code = 0;
   rc = cellHttpResponseGetStatusCode(*trans, &code);
   uint64_t tAfterStatus = sys_time_get_system_time();
   if (rc < 0) { logError("[http-fs] getStatusCode failed, rc=0x%x\n", rc); return -1; }
   if (code != 200 && code != 206) { logError("[http-fs] http status %d\n", code); return -1; }

   logInfo("[http-fs] diag open off=%llu setup(conn+tls+send)=%llums firstResponse=%llums code=%d\n",
           (unsigned long long)offset, (unsigned long long)((tAfterSend - tBeforeSend) / 1000),
           (unsigned long long)((tAfterStatus - tAfterSend) / 1000), code);

   if (sizeOut) { uint64_t length = 0; *sizeOut = cellHttpResponseGetContentLength(*trans, &length) == 0 ? length : 0; }
   return 0;
}

// copy n bytes at file offset from the ring into dst (handles the wrap). caller holds stateLock.
static void copyFromRing(const HttpStream *stream, uint64_t offset, void *dst, uint64_t n)
{
   size_t slot = (size_t)(offset % RING_SIZE);
   size_t firstPart = RING_SIZE - slot;
   if (firstPart >= n) {
      memcpy(dst, stream->ring + slot, (size_t)n);
   } else {
      memcpy(dst, stream->ring + slot, firstPart);
      memcpy((uint8_t *)dst + firstPart, stream->ring, (size_t)(n - firstPart));
   }
}

// section: prefetch thread - keeps the ring filled ahead of the reader

static void prefetchThread(uint64_t arg)
{
   HttpStream *stream = (HttpStream *)(uintptr_t)arg;

   while (stream->running) {
      if (stream->error) { sleepMs(20); continue; }   // latched failure: idle until the reader closes us

      // snapshot the control state
      lock(&stream->stateLock);
      uint64_t position = stream->position, ringStart = stream->ringStart, ringEnd = stream->ringEnd;
      int needReconnect = stream->needReconnect;
      unlock(&stream->stateLock);

      // reconnect when the reader seeked off the window (or nothing is open yet)
      if (needReconnect || !stream->trans || position < ringStart || position > ringEnd + FORWARD_RECONNECT) {
         if (stream->trans && (position < ringStart || position > ringEnd + FORWARD_RECONNECT))
            logInfo("[http-fs] seek reconnect to offset %llu\n", (unsigned long long)position);   // counts the cold range requests a seek costs
         if (stream->diagSeekReqUs) {   // TEMP: time from the seek asking for a reconnect to us acting on it (should be ~0 unless stuck in recv)
            logInfo("[http-fs] diag reconnect reaction %llums\n", (unsigned long long)((sys_time_get_system_time() - stream->diagSeekReqUs) / 1000));
            stream->diagSeekReqUs = 0;
         }
         lockNet();
         int rc = openTransaction(stream->client, stream->url, position, &stream->trans, &stream->uriPool, NULL);
         unlockNet();

         // on failure openTransaction leaves a dead (created-but-unsent, or non-2xx) transaction set, so
         // keep needReconnect=1 to force a real reopen next iteration (it tears that stale trans down first)
         // rather than falling through and recv'ing the error body into the ring as media.
         lock(&stream->stateLock);
         stream->atEof = 0;
         if (rc == 0) {
            stream->ringStart = stream->ringEnd = position;
            stream->needReconnect = 0;
            stream->errorRetries = 0;
            stream->diagRecvBudget = 8;   // TEMP: watch the first recvs on the fresh connection
         } else if (stream->errorRetries + 1 >= PREFETCH_ERROR_RETRIES) {
            stream->error = 1;                 // out of retries: give up
         } else {
            stream->errorRetries++;
            stream->needReconnect = 1;
         }
         unlock(&stream->stateLock);
         if (rc != 0) sleepMs(100);            // back off before the retry
         continue;
      }

      // already buffered far enough ahead, or done: idle. (guard the unsigned subtraction: a small
      // forward seek can leave position past ringEnd, which must count as zero buffered, not wrap)
      uint64_t ahead = ringEnd > position ? ringEnd - position : 0;
      if (stream->atEof || ahead >= PREFETCH_AHEAD) { sleepMs(4); continue; }

      // fetch one chunk into the ring just past ringEnd (a region the reader never touches until
      // ringEnd advances), then publish it under the lock
      size_t slot = (size_t)(ringEnd % RING_SIZE);
      size_t contiguous = RING_SIZE - slot;
      size_t chunk = contiguous < PREFETCH_CHUNK ? contiguous : PREFETCH_CHUNK;

      // recv UNLOCKED: this transaction is only ever touched by this thread, and holding netLock here
      // starved the other stream whenever the server trickled (see the lock notes at the top)
      size_t got = 0;
      uint64_t tRecv0 = sys_time_get_system_time();   // TEMP
      int rc = cellHttpRecvResponse(stream->trans, stream->ring + slot, chunk, &got);
      if (stream->diagRecvBudget > 0) {   // TEMP: got==chunk => fill-bound (Fix1 helps); got<chunk => available-bound. elapsed small+got big early => reburst
         stream->diagRecvBudget--;
         logInfo("[http-fs] diag recv got=%u/%u elapsed=%llums rc=0x%x\n", (unsigned)got, (unsigned)chunk,
                 (unsigned long long)((sys_time_get_system_time() - tRecv0) / 1000), rc);
      }

      lock(&stream->stateLock);
      if (got > 0) {   // commit whatever arrived, whether the call ended ok or timed out mid-chunk
         stream->ringEnd += got;
         if (stream->ringEnd - stream->ringStart > RING_SIZE) stream->ringStart = stream->ringEnd - RING_SIZE;
         stream->errorRetries = 0;
      } else if (rc == (int)CELL_HTTP_ERROR_NET_SELECT_TIMEOUT) {
         // stream stalled this turn; the other stream isn't blocked (recv is unlocked), just retry
      } else if (rc < 0) {
         // transient network error (reset/dropped connection): reconnect at the current window end
         // instead of latching a permanent error - one bad recv used to kill audio for a whole play
         if (stream->errorRetries < PREFETCH_ERROR_RETRIES) {
            stream->errorRetries++;
            stream->needReconnect = 1;
            logWarn("[http-fs] recv failed rc=0x%x, reconnect attempt %d\n", rc, stream->errorRetries);
         } else {
            stream->error = 1;
         }
      } else {
         stream->atEof = 1;   // got == 0 with no error: end of this stream
      }
      unlock(&stream->stateLock);
   }
   exitThread();
}

// section: VfsOps

// drops an unclaimed probe connection (client + transaction). caller holds netLock.
static void dropPendingOpen(void)
{
   if (!pendingOpen.valid) return;
   destroyConnection(&pendingOpen.client, &pendingOpen.trans, &pendingOpen.uriPool);
   pendingOpen.valid = 0;
}

static int statHttp(const char *url, VfsStat *outStat)
{
   memset(outStat, 0, sizeof *outStat);

   if (ensureHttpStack() != 0) return -1;
   lockNet();
   dropPendingOpen();   // discard any earlier unclaimed probe first

   // open the connection here on its own client and keep it for the openHttp that follows, so stat+open
   // cost one handshake. openHttp for this url adopts it; anything else drops it. The first HTTPS to a
   // fresh host occasionally fails at sendRequest (rc=0x80710d23) and works on a retry, so try a few
   // times with a fresh client each attempt - otherwise that one blip drops audio to silent for the
   // whole video (the "first video of a session had no sound" bug).
   int ok = 0;
   for (int attempt = 0; attempt < STREAM_OPEN_ATTEMPTS && !ok; attempt++) {
      if (attempt > 0) {
         destroyConnection(&pendingOpen.client, &pendingOpen.trans, &pendingOpen.uriPool);
         logWarn("[http-fs] probe open failed, retry %d/%d\n", attempt, STREAM_OPEN_ATTEMPTS - 1);
         sleepMs(100);
      }
      ok = createStreamClient(&pendingOpen.client) == 0 &&
           openTransaction(pendingOpen.client, url, 0, &pendingOpen.trans, &pendingOpen.uriPool, &pendingOpen.size) == 0;
   }
   if (ok) {
      strCopy(pendingOpen.url, sizeof pendingOpen.url, url);
      pendingOpen.valid = 1;
      outStat->size = pendingOpen.size;
   } else {
      destroyConnection(&pendingOpen.client, &pendingOpen.trans, &pendingOpen.uriPool);
   }
   unlockNet();
   return ok ? 0 : -1;
}

static int openHttp(const char *url, int flags, VfsFile *file)
{
   if (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) return -1;   // read-only

   uint8_t *ring = malloc(RING_SIZE);
   if (!ring) return -1;

   lockNet();
   HttpStream *stream = NULL;
   for (int i = 0; i < MAX_STREAMS; i++)
      if (!streams[i].inUse) { stream = &streams[i]; file->descriptor = i; break; }
   if (!stream) { dropPendingOpen(); unlockNet(); free(ring); return -1; }   // release the unclaimed probe

   // adopt the probe connection statHttp opened for this url; otherwise the prefetch thread opens the
   // connection at position 0. The stack is already up (statHttp brought it up before this).
   int adopt = pendingOpen.valid && strcmp(pendingOpen.url, url) == 0;
   if (!adopt) dropPendingOpen();

   memset(stream, 0, sizeof *stream);
   stream->inUse = 1;
   stream->ring  = ring;
   strCopy(stream->url, sizeof stream->url, url);
   if (adopt) {
      stream->client = pendingOpen.client;  stream->trans = pendingOpen.trans;
      stream->uriPool = pendingOpen.uriPool; stream->size = pendingOpen.size;
      // the stream owns these now: clear the aliases so a later statHttp doesn't destroy/free them a
      // second time (they get freed when this stream closes). This double-free crashed the 2nd play.
      pendingOpen.client = 0; pendingOpen.trans = 0; pendingOpen.uriPool = NULL;
      pendingOpen.valid = 0;
   } else if (createStreamClient(&stream->client) != 0) {   // own client; prefetch opens the connection at 0
      stream->inUse = 0; unlockNet(); free(ring); return -1;
   } else {
      stream->needReconnect = 1;
   }
   unlockNet();

   createLock(&stream->stateLock);
   stream->running = 1;
   stream->prefetchActive = spawnJoinableThread(&stream->prefetchThread, prefetchThread, (uint64_t)(uintptr_t)stream,
                            THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "http-fs") == 0;
   if (!stream->prefetchActive) { closeHttp(file); return -1; }   // no prefetch = reads would hang
   return 0;
}

static int64_t readHttp(VfsFile *file, void *buffer, uint64_t length)
{
   HttpStream *stream = &streams[file->descriptor];

   // wait for the prefetch thread to buffer [position, position+length), unless it hits EOF/error
   for (;;) {
      lock(&stream->stateLock);
      if (!stream->inUse) { unlock(&stream->stateLock); return -1; }

      // reader moved off the buffered window (backward past the history): ask the prefetch to reopen
      if (stream->position < stream->ringStart) stream->needReconnect = 1;

      // serve whatever the ring already holds for this position (callers loop on short reads); a
      // single request can exceed PREFETCH_AHEAD, so never wait for the *full* length. data is only
      // in the ring when position is within [ringStart, ringEnd) - outside it, wait for the prefetch.
      uint64_t available = 0;
      if (stream->position >= stream->ringStart && stream->ringEnd > stream->position)
         available = stream->ringEnd - stream->position;
      int failed = stream->error;
      if ((available > 0 || stream->atEof) && !failed) {
         uint64_t served = available < length ? available : length;
         if (served > 0) copyFromRing(stream, stream->position, buffer, served);
         stream->position += served;
         unlock(&stream->stateLock);
         return (int64_t)served;
      }
      unlock(&stream->stateLock);
      if (failed) return -1;
      sleepMs(2);   // nothing buffered yet; let the prefetch thread catch up
   }
}

static int64_t seekHttp(VfsFile *file, int64_t offset, int whence)
{
   HttpStream *stream = &streams[file->descriptor];

   lock(&stream->stateLock);
   if (!stream->inUse) { unlock(&stream->stateLock); return -1; }
   uint64_t base;
   if      (whence == VFS_SEEK_SET) base = 0;
   else if (whence == VFS_SEEK_CUR) base = stream->position;
   else if (whence == VFS_SEEK_END) base = stream->size;
   else { unlock(&stream->stateLock); return -1; }
   int64_t target = (int64_t)base + offset;
   if (target < 0) { unlock(&stream->stateLock); return -1; }
   stream->position = (uint64_t)target;
   // a seek to data not already in the ring: reconnect at the target now. Without this, a forward seek that
   // lands just past the buffered end (inside FORWARD_RECONNECT) is mistaken for sequential drift, and the
   // prefetch coasts up to it at the server's throttled media bitrate - on a trickled audio stream (~6 KB/s)
   // even a small gap then takes tens of seconds. Only genuine seeks call this (sequential reads never do).
   if (stream->position < stream->ringStart || stream->position > stream->ringEnd) {
      stream->needReconnect = 1;
      stream->diagSeekReqUs = sys_time_get_system_time();   // TEMP: measure how fast the prefetch reacts
   }
   unlock(&stream->stateLock);
   return target;
}

static int closeHttp(VfsFile *file)
{
   int slot = file->descriptor;
   if (slot >= 0 && slot < MAX_STREAMS && streams[slot].inUse) {
      HttpStream *stream = &streams[slot];
      stream->running = 0;
      if (stream->prefetchActive) { joinThread(stream->prefetchThread); stream->prefetchActive = 0; }
      destroyLock(&stream->stateLock);

      lockNet();
      destroyConnection(&stream->client, &stream->trans, &stream->uriPool);   // stack stays resident
      unlockNet();

      free(stream->ring);
      stream->ring = NULL;
      stream->inUse = 0;
   }
   file->descriptor = -1;
   return 0;
}

// unsupported operations: this backend is a read-only url stream, not a tree.
static int     renameHttp(const char *from, const char *to)                 { (void)from; (void)to; return -1; }
static int     mkdirHttp(const char *native)                                { (void)native; return -1; }
static int     rmfileHttp(const char *native)                               { (void)native; return -1; }
static int     rmdirHttp(const char *native)                                { (void)native; return -1; }
static int     getfreeHttp(const char *native, uint64_t *freeB, uint64_t *totalB) { (void)native; if (freeB) *freeB = 0; if (totalB) *totalB = 0; return -1; }
static int     opendirHttp(const char *native, VfsDir *dir)                 { (void)native; (void)dir; return -1; }
static int     readdirHttp(VfsDir *dir, char *nameOut, int cap, VfsEntryType *typeOut) { (void)dir; (void)nameOut; (void)cap; (void)typeOut; return -1; }
static void    closedirHttp(VfsDir *dir)                                    { (void)dir; }
static int64_t writeHttp(VfsFile *file, const void *buffer, uint64_t length){ (void)file; (void)buffer; (void)length; return -1; }
static int     fsyncHttp(VfsFile *file)                                     { (void)file; return 0; }

static const VfsOps HTTP_OPS = {
   statHttp, renameHttp, mkdirHttp, rmfileHttp, rmdirHttp, getfreeHttp,
   opendirHttp, readdirHttp, closedirHttp,
   openHttp, readHttp, writeHttp, seekHttp, fsyncHttp, closeHttp,
};

void initHttpFs(void)
{
   if (!locksReady) { createLock(&netLock); locksReady = 1; }
   setUrlVfsBackend(&HTTP_OPS);
}

void termHttpFs(void)
{
   setUrlVfsBackend(NULL);
   lockNet();
   dropPendingOpen();   // release any unclaimed probe (client + transaction + libhttp ref)
   unlockNet();
}
