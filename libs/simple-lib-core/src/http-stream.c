// http-stream - the seekable HTTP(S) streaming engine behind openHttpStream (see http.h).
//
// Each open gets its own 4 MB ring buffer and a background prefetch thread that keeps the ring filled ahead
// of the reader, so a network dip drains the buffer instead of stalling playback; a seek off the buffered
// window makes the prefetch reconnect with a fresh range request. One per-stream stateLock guards the ring
// window and read position. This TU owns all the streaming's malloc + threads and is pulled ONLY when a
// caller references openHttpStream — the light one-shot path in http.c stays free of it (see http-internal.h).

#include "http.h"
#include "http-internal.h"       // DEFAULT_UA, getActiveTransport
#include "thread.h"              // lock + thread helpers
#include "string-utilities.h"    // strCopy
#include "dbg.h"

#include <stdlib.h>
#include <string.h>
#include <sys/sys_time.h>   // sys_time_get_system_time (slow-transfer diagnostics)

#define HTTP_URL_MAX 2048
#define MAX_STREAMS  4

// the ring keeps a window of streamed bytes indexed by file offset % RING_SIZE. the prefetch thread fills
// it forward to PREFETCH_AHEAD past the read position; the rest is history for small back-reads. A read
// position more than FORWARD_RECONNECT past the window is treated as a seek.
#define RING_SIZE         (4 * 1024 * 1024)
#define PREFETCH_AHEAD    (2 * 1024 * 1024)
#define PREFETCH_CHUNK    (8 * 1024)   // bytes per recv step; small keeps seek-reconnect reaction snappy
#define FORWARD_RECONNECT (1 * 1024 * 1024)
#define SEEK_COAST_BYTES  (256 * 1024)   // a forward seek gap this small streams through faster than a reconnect
#define PREFETCH_ERROR_RETRIES 3       // reconnects to try on a failed recv/open before latching an error

// a query-range host (see usesQueryRange) serves only so far ahead of playback: measured on 2026-08-19, a
// first request covering the next 65 seconds of a stream is served and the one after it refused until
// playback catches up. so on those hosts the window and the prefetch are both sized in seconds of media,
// from the stream's own byte rate, and a refusal is a "come back later" rather than a failure.
#define RANGE_WINDOW       (4 * 1024 * 1024)   // upper bound on one request, whatever the byte rate says
#define PREFETCH_SECONDS   20   // media the prefetch may run ahead of the reader
#define WINDOW_SECONDS     10   // media one request asks for
#define REFUSED_BACKOFF_MS 500
#define REFUSED_RETRIES    120  // a minute of being told to come back later before giving up on the stream

// prefetch-thread idle/backoff waits, in milliseconds.
#define ERROR_IDLE_MS        20   // latched failure: idle until the reader closes the stream
#define RECONNECT_BACKOFF_MS 100  // wait after a failed reopen before trying again
#define PREFETCH_IDLE_MS     4    // buffered far enough ahead (or at eof): brief idle
#define READER_WAIT_MS       2    // reader has nothing buffered yet: let the prefetch catch up

struct HttpStream {
   int              inUse;
   char             url[HTTP_URL_MAX];
   uint8_t         *ring;
   uint64_t         position;      // reader's logical read position (guarded by stateLock)
   uint64_t         ringStart;     // lowest file offset still in the ring (prefetch advances it)
   uint64_t         ringEnd;       // one past the highest streamed byte
   uint64_t         size;          // total resource size (0 if unknown)
   void            *conn;          // this stream's transport connection (prefetch-thread owned)
   uint64_t         connEnd;       // one past the last byte the open connection serves (prefetch-thread owned)
   int              rangeInQuery;  // ask for the byte window in the url instead of a Range header (see usesQueryRange)
   uint64_t         bytesPerSecond; // the media's own byte rate, 0 if unknown: paces requests on a query-range host
   int              refusals;      // consecutive "come back later" refusals (reset on a served request)
   int              needReconnect; // reader -> prefetch: position moved off the window, reopen there
   int              errorRetries;  // consecutive failed recv/reconnect attempts (reset on progress)
   volatile int     running;       // prefetch thread runs while set
   volatile int     atEof;         // stream reached its end
   volatile int     error;         // a transaction/recv failed beyond retry
   int              lockReady;     // stateLock created
   sys_lwmutex_t    stateLock;     // guards ring window + position + flags
   sys_ppu_thread_t prefetchThread;
   int              prefetchActive;
};

static HttpStream   streams[MAX_STREAMS];
static sys_lwmutex_t streamsLock;         // guards the streams[] slot registry (open/close)
static volatile int streamsLockState;    // 0 uninit, 1 creating, 2 ready

// create the registry lock exactly once, safe against concurrent first opens (video + audio can open at
// once). unlike the transport inits, openHttpStream is not a single-threaded startup hook, so elect one
// creator with a compare-and-swap and let late-comers wait for it.
static void ensureStreamsLock(void)
{
   if (streamsLockState == 2) return;
   if (__sync_bool_compare_and_swap(&streamsLockState, 0, 1)) {
      createLock(&streamsLock);
      __sync_synchronize();
      streamsLockState = 2;
   } else {
      while (streamsLockState != 2) yieldThread();
   }
}

// appends value's decimal digits to out without pulling in libc's snprintf (core stays libc-free).
static void appendNumber(char *out, int cap, int *length, uint64_t value)
{
   char digits[24];
   int digitCount = 0;
   if (value == 0) digits[digitCount++] = '0';
   else for (; value > 0; value /= 10) digits[digitCount++] = (char)('0' + (int)(value % 10));
   for (int i = 0; i < digitCount && *length < cap - 1; i++) out[(*length)++] = digits[digitCount - 1 - i];
}

// writes "bytes=<offset>-" into out.
static void formatRange(char *out, int cap, uint64_t offset)
{
   int length = 0;
   for (const char *prefix = "bytes="; *prefix && length < cap - 1; prefix++) out[length++] = *prefix;
   appendNumber(out, cap, &length, offset);
   if (length < cap - 1) out[length++] = '-';
   out[length] = '\0';
}

// writes url + "&range=<start>-<end>" into out. 0 if it does not fit.
static int formatQueryRange(char *out, int cap, const char *url, uint64_t start, uint64_t end)
{
   int length = 0;
   while (url[length] && length < cap - 1) { out[length] = url[length]; length++; }
   for (const char *parameter = "&range="; *parameter && length < cap - 1; parameter++) out[length++] = *parameter;
   appendNumber(out, cap, &length, start);
   if (length < cap - 1) out[length++] = '-';
   appendNumber(out, cap, &length, end);
   out[length] = '\0';
   return length < cap - 1;
}

// reads the value of a numeric query parameter, e.g. getQueryNumber(url, "clen=") -> the resource size. 0 if absent.
static uint64_t getQueryNumber(const char *url, const char *parameter)
{
   const char *hit = strstr(url, parameter);
   if (!hit) return 0;
   uint64_t value = 0;
   for (const char *digit = hit + strlen(parameter); *digit >= '0' && *digit <= '9'; digit++)
      value = value * 10 + (uint64_t)(*digit - '0');
   return value;
}

// youtube's media hosts stopped serving the Range header on 2026-08-19: a ranged GET comes back as a 302
// whose target then 403s, so nothing plays. their own clients ask for the window as a "&range=start-end"
// query parameter instead and that still works, one bounded window per request, with the resource size
// carried in the url as clen= because the response no longer reports it.
static int usesQueryRange(const char *url) { return strstr(url, "googlevideo.com") != NULL; }

// open a GET at offset via the bound transport: one range window on a query-range host, otherwise everything
// from offset on. returns the connection, or NULL (also on non-2xx), and sets the stream's connEnd.
static void *openConnection(HttpStream *stream, uint64_t offset, uint64_t *totalSize)
{
   const HttpTransport *transport = getActiveTransport();

   // the request: one window in the query on a query-range host, everything from offset on anywhere else.
   // headers[1] is only sent in the second case, hence the count.
   char request[HTTP_URL_MAX];
   char range[48];
   HttpHeader headers[2] = { { "User-Agent", DEFAULT_UA }, { "Range", range } };
   int headerCount = 1;
   uint64_t connEnd = (uint64_t)-1;

   if (stream->rangeInQuery) {
      uint64_t window = stream->bytesPerSecond ? stream->bytesPerSecond * WINDOW_SECONDS : RANGE_WINDOW;
      if (window > RANGE_WINDOW) window = RANGE_WINDOW;
      uint64_t windowEnd = offset + window - 1;
      if (stream->size && windowEnd >= stream->size) windowEnd = stream->size - 1;   // a range past the end is refused
      if (!formatQueryRange(request, sizeof request, stream->url, offset, windowEnd)) {
         logError("[http] url too long for a query range\n");
         return NULL;
      }
      connEnd = windowEnd + 1;
   } else {
      strCopy(request, sizeof request, stream->url);
      formatRange(range, sizeof range, offset);
      headerCount = 2;
   }

   // send it. a 403 on a query-range host means "you are too far ahead of playback", not a dead url.
   int status = 0;
   void *conn = transport->open("GET", request, headers, headerCount, NULL, 0, &status, totalSize);
   int refused = conn && status == 403 && stream->rangeInQuery;
   stream->refusals = refused ? stream->refusals + 1 : 0;

   if (conn && status != 200 && status != 206) {
      if (!refused) {
         // log host + path only: a googlevideo query string runs to ~2 KB and overruns the log line buffer
         char endpoint[128];
         int i = 0;
         while (stream->url[i] && stream->url[i] != '?' && i < (int)sizeof endpoint - 1) { endpoint[i] = stream->url[i]; i++; }
         endpoint[i] = '\0';
         logError("[http] status %d streaming %s at offset %llu\n", status, endpoint, (unsigned long long)offset);
      }
      transport->close(conn);
      conn = NULL;
   }
   if (!conn) return NULL;

   stream->connEnd = connEnd;
   return conn;
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

static void prefetchThread(uint64_t arg)
{
   HttpStream *stream = (HttpStream *)(uintptr_t)arg;
   const HttpTransport *transport = getActiveTransport();

   while (stream->running) {
      if (stream->error) { sleepMs(ERROR_IDLE_MS); continue; }   // latched failure: idle until the reader closes us

      lock(&stream->stateLock);
      uint64_t position = stream->position, ringStart = stream->ringStart, ringEnd = stream->ringEnd;
      int needReconnect = stream->needReconnect;
      unlock(&stream->stateLock);

      // the last window ended at the end of the resource: nothing left to fetch
      int windowSpent = stream->conn && ringEnd >= stream->connEnd;
      if (windowSpent && stream->size && ringEnd >= stream->size) {
         lock(&stream->stateLock);
         if (!stream->needReconnect) stream->atEof = 1;
         unlock(&stream->stateLock);
         sleepMs(PREFETCH_IDLE_MS);
         continue;
      }

      // reopen when the reader seeked off the window, or when this connection's range window is used up
      int seeked = needReconnect || !stream->conn || position < ringStart || position > ringEnd + FORWARD_RECONNECT;
      if (seeked || windowSpent) {
         uint64_t offset = seeked ? position : ringEnd;   // a spent window carries on where it ended; a seek restarts the ring
         if (stream->conn) { transport->close(stream->conn); stream->conn = NULL; }

         uint64_t tReconnect = sys_time_get_system_time();
         void *conn = openConnection(stream, offset, NULL);
         if (conn && seeked) logInfo("[http] reconnect at offset %llu took %llums\n", (unsigned long long)offset,
                                     (unsigned long long)((sys_time_get_system_time() - tReconnect) / 1000));

         // refused for running ahead of playback: leave the ring alone and ask again once the reader has drained it
         if (!conn && stream->refusals) {
            if (stream->refusals < REFUSED_RETRIES) { sleepMs(REFUSED_BACKOFF_MS); continue; }
            logError("[http] refused %d times at offset %llu, giving up\n", stream->refusals, (unsigned long long)offset);
         }

         lock(&stream->stateLock);
         stream->atEof = 0;
         if (conn) {
            stream->conn = conn;
            if (seeked) stream->ringStart = stream->ringEnd = offset;
            stream->needReconnect = 0;
            stream->errorRetries = 0;
         } else if (stream->errorRetries + 1 >= PREFETCH_ERROR_RETRIES) {
            stream->error = 1;
         } else {
            stream->errorRetries++;
            stream->needReconnect = 1;
         }
         unlock(&stream->stateLock);
         if (!conn) sleepMs(RECONNECT_BACKOFF_MS);
         continue;
      }

      // already buffered far enough ahead, or done: idle. (guard the unsigned subtraction)
      uint64_t aheadLimit = stream->bytesPerSecond ? stream->bytesPerSecond * PREFETCH_SECONDS : PREFETCH_AHEAD;
      if (aheadLimit > PREFETCH_AHEAD) aheadLimit = PREFETCH_AHEAD;
      uint64_t ahead = ringEnd > position ? ringEnd - position : 0;
      if (stream->atEof || ahead >= aheadLimit) { sleepMs(PREFETCH_IDLE_MS); continue; }

      // fetch one chunk into the ring just past ringEnd, then publish it under the lock
      size_t slot = (size_t)(ringEnd % RING_SIZE);
      size_t contiguous = RING_SIZE - slot;
      size_t chunk = contiguous < PREFETCH_CHUNK ? contiguous : PREFETCH_CHUNK;
      uint64_t windowLeft = stream->connEnd - ringEnd;   // never read past what this connection was asked for
      if (windowLeft < chunk) chunk = (size_t)windowLeft;
      uint64_t tRead = sys_time_get_system_time();
      int64_t got = transport->read(stream->conn, stream->ring + slot, chunk);
      uint64_t readMs = (sys_time_get_system_time() - tRead) / 1000;
      if (readMs >= 2000)   // a normal chunk arrives in milliseconds; this is the "video stuck loading" shape
         logWarn("[http] slow recv: %llums for a %d-byte chunk at offset %llu\n",
                 (unsigned long long)readMs, (int)chunk, (unsigned long long)ringEnd);

      lock(&stream->stateLock);
      if (got > 0) {
         stream->ringEnd += (uint64_t)got;
         if (stream->ringEnd - stream->ringStart > RING_SIZE) stream->ringStart = stream->ringEnd - RING_SIZE;
         stream->errorRetries = 0;
      } else if (got == -2) {
         // stalled this turn but the connection is alive; just retry
      } else if (got == -1) {
         if (stream->errorRetries < PREFETCH_ERROR_RETRIES) {
            stream->errorRetries++;
            stream->needReconnect = 1;
            logWarn("[http] recv failed, reconnect attempt %d\n", stream->errorRetries);
         } else {
            stream->error = 1;
         }
      } else if (!stream->needReconnect) {
         // got == 0: the connection ended. that is the end of the resource only when we have streamed all of
         // it; a connection that stops short of its window reopens there. ignore it when a seek has already
         // asked for a reconnect - the zero was the pre-seek connection's end, not the new position's.
         if (stream->size && stream->ringEnd < stream->size && stream->errorRetries < PREFETCH_ERROR_RETRIES) {
            stream->errorRetries++;
            stream->needReconnect = 1;
         } else {
            stream->atEof = 1;
         }
      }
      unlock(&stream->stateLock);
   }
   exitThread();
}

HttpStream *openHttpStream(const char *url)
{
   if (!getActiveTransport()) { logError("[http] no transport bound\n"); return NULL; }

   uint8_t *ring = malloc(RING_SIZE);
   if (!ring) return NULL;

   ensureStreamsLock();
   lock(&streamsLock);
   HttpStream *stream = NULL;
   for (int i = 0; i < MAX_STREAMS; i++)
      if (!streams[i].inUse) { stream = &streams[i]; break; }
   if (!stream) { unlock(&streamsLock); free(ring); return NULL; }
   memset(stream, 0, sizeof *stream);
   stream->inUse = 1;
   unlock(&streamsLock);

   stream->ring = ring;
   strCopy(stream->url, sizeof stream->url, url);
   createLock(&stream->stateLock);
   stream->lockReady = 1;

   // open synchronously so the size is known on return and the prefetch starts already connected
   uint64_t totalSize = 0;
   stream->rangeInQuery = usesQueryRange(url);
   if (stream->rangeInQuery) {
      stream->size = getQueryNumber(url, "clen=");   // known before the first request, and needed to clamp it
      uint64_t durationSeconds = getQueryNumber(url, "dur=");
      if (durationSeconds) stream->bytesPerSecond = stream->size / durationSeconds;
   }
   stream->conn = openConnection(stream, 0, &totalSize);
   if (!stream->conn) { closeHttpStream(stream); return NULL; }
   if (!stream->size) stream->size = totalSize;

   stream->running = 1;
   stream->prefetchActive = spawnJoinableThread(&stream->prefetchThread, prefetchThread, (uint64_t)(uintptr_t)stream,
                            THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "http-stream") == 0;
   if (!stream->prefetchActive) { closeHttpStream(stream); return NULL; }
   return stream;
}

int64_t readHttpStream(HttpStream *stream, void *buffer, uint64_t length)
{
   int starvedMs = 0;   // how long this call has waited on an empty ring (diagnoses a stalled stream)
   for (;;) {
      lock(&stream->stateLock);
      if (!stream->inUse) { unlock(&stream->stateLock); return -1; }

      // reader moved off the buffered window (backward past the history): ask the prefetch to reopen. clear
      // atEof too - it described the old position; the reconnect re-establishes end-of-stream at the new one.
      if (stream->position < stream->ringStart) { stream->needReconnect = 1; stream->atEof = 0; }

      // serve whatever the ring already holds for this position (callers loop on short reads)
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
      sleepMs(READER_WAIT_MS);   // nothing buffered yet; let the prefetch thread catch up
      starvedMs += READER_WAIT_MS;
      if (starvedMs >= 3000) {   // repeats every 3s while starved, so a long stall leaves a trail in the log
         logWarn("[http] reader starved %dms at offset %llu\n", starvedMs, (unsigned long long)stream->position);
         starvedMs = 0;
      }
   }
}

int seekHttpStream(HttpStream *stream, uint64_t offset)
{
   lock(&stream->stateLock);
   if (!stream->inUse) { unlock(&stream->stateLock); return -1; }
   stream->position = offset;
   // a seek to data not already in the ring: reconnect at the target (a large forward seek would
   // otherwise coast up at the server's throttled bitrate). a SMALL forward gap is the exception -
   // the prefetch streams through it in a few chunks, cheaper than a full https round trip (seeks
   // land the audio stream a couple of seconds short of the target, which is exactly this shape).
   // clear atEof with a reconnect: a stale end flag would return a spurious 0 before it lands.
   if (stream->position < stream->ringStart || (stream->position > stream->ringEnd + SEEK_COAST_BYTES && !stream->atEof)) {
      stream->needReconnect = 1;
      stream->atEof = 0;
   }
   unlock(&stream->stateLock);
   return 0;
}

uint64_t getHttpStreamSize(HttpStream *stream) { return stream ? stream->size : 0; }

void closeHttpStream(HttpStream *stream)
{
   if (!stream || !stream->inUse) return;
   stream->running = 0;
   if (stream->prefetchActive) { joinThread(stream->prefetchThread); stream->prefetchActive = 0; }
   if (stream->lockReady) { destroyLock(&stream->stateLock); stream->lockReady = 0; }
   if (stream->conn) { getActiveTransport()->close(stream->conn); stream->conn = NULL; }
   free(stream->ring);
   stream->ring = NULL;

   lock(&streamsLock);
   stream->inUse = 0;
   unlock(&streamsLock);
}
