// live-source - continuous fragmented-mp4 stream over a DASH live sq/<n> segment sequence (see live-source.h).
//
// A background prefetch thread fetches segments ahead into a rolling window, so the demuxer/decoder reading
// the window never blocks on a segment download - without it, playback stalls once per segment and then
// bursts to catch up. One lock guards the window (producer = prefetch, consumer = the reading decode thread).

#include "live-source.h"
#include "http.h"
#include "string-utilities.h"   // strCopy
#include "thread.h"             // lock + thread helpers
#include "dbg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SEGMENT_FETCH_CAP (8 * 1024 * 1024)    // one segment (a few seconds of 1080p); keyframe segments fit
#define WINDOW_CAP        (8 * 1024 * 1024)    // rolling concatenated-stream buffer kept for the demuxer
#define WINDOW_BACKLOG    (2 * 1024 * 1024)    // history kept before the read position for small back-seeks
#define TARGET_AHEAD      (4 * 1024 * 1024)    // how far ahead of the reader the prefetch keeps the window filled
#define START_BEHIND_EDGE 3                    // begin this many segments back from the edge for a small buffer
#define EDGE_POLL_MS      250                  // wait between retries for the next segment at the live edge
#define RECONNECT_MS      200                  // backoff after a failed segment fetch
#define PREFETCH_IDLE_MS  8                    // buffered far enough ahead: brief idle
#define READ_WAIT_MS      2                    // reader has nothing buffered yet: let the prefetch catch up

// googlevideo segment urls are signed, so any user-agent is accepted.
static const char *SEGMENT_UA = "Mozilla/5.0 (PLAYSTATION 3; 4.9)";

struct LiveSource {
   char     base[2048];   // "<...>/" - append "sq/<n>"
   long     nextSq;       // next segment number to fetch (prefetch-thread owned)
   int      firstDone;    // the first (init-bearing) segment has been emitted
   int      failStreak;   // consecutive failed fetches of the current segment, for log throttling

   uint8_t *window;       // rolling buffer of the virtual stream (guarded by lock)
   int      windowLen;
   uint64_t windowStart;  // virtual offset of window[0]
   uint64_t position;     // current read position (virtual)

   uint8_t *fetchBuffer;  // reused per-segment download buffer (prefetch-thread owned)

   sys_lwmutex_t    lock;
   int              lockReady;
   sys_ppu_thread_t prefetchThread;
   int              threadActive;
   volatile int     running;
   volatile int     cancel;
};

// offset of the first 'moof' box (skipping a segment's leading ftyp/styp/sidx/moov init). 0 if none found.
static int firstMoofOffset(const uint8_t *data, int length)
{
   uint32_t pos = 0;
   while (pos + 8 <= (uint32_t)length) {
      uint32_t size = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
      if (data[pos + 4] == 'm' && data[pos + 5] == 'o' && data[pos + 6] == 'o' && data[pos + 7] == 'f') return (int)pos;
      if (size < 8) break;
      pos += size;
   }
   return 0;
}

// append `length` bytes to the window, first dropping already-read history (keeping a small backlog) so it
// stays bounded. caller holds the lock.
static void appendWindow(LiveSource *source, const uint8_t *data, int length)
{
   if (source->windowLen + length > WINDOW_CAP) {
      uint64_t keepFrom = source->position > source->windowStart + WINDOW_BACKLOG ? source->position - WINDOW_BACKLOG : source->windowStart;
      int drop = (int)(keepFrom - source->windowStart);
      if (drop > 0) {
         memmove(source->window, source->window + drop, source->windowLen - drop);
         source->windowLen -= drop;
         source->windowStart += drop;
      }
   }
   if (source->windowLen + length > WINDOW_CAP) length = WINDOW_CAP - source->windowLen;   // never overrun
   if (length <= 0) return;
   memcpy(source->window + source->windowLen, data, length);
   source->windowLen += length;
}

// fetch the next segment (network, no lock) and append it init-stripped (under lock). 0 ok, 1 edge, -1 error.
static int fetchNextSegment(LiveSource *source)
{
   char url[2200];
   snprintf(url, sizeof url, "%ssq/%ld", source->base, source->nextSq);
   HttpHeader headers[] = { { "User-Agent", SEGMENT_UA }, { "Accept-Encoding", "identity" } };

   int length = 0, status = 0;
   int rc = fetchHttp("GET", url, headers, 2, NULL, 0, (char *)source->fetchBuffer, SEGMENT_FETCH_CAP, &length, &status);
   if (rc != 0 || length <= 0) {
      if (rc == 0 && (status == 204 || status == 404)) return 1;   // not generated yet: we're at the live edge
      // the caller retries the same segment every 200ms, so a network outage would log 5 lines a second
      // forever; log the first failure and then one per ~5s of outage
      if (source->failStreak++ % 25 == 0)
         logError("[live] segment sq/%ld failed rc=%d status=%d (fail #%d)\n", source->nextSq, rc, status, source->failStreak);
      return -1;
   }
   if (status != 200 && status != 206) return (status == 204 || status == 404) ? 1 : -1;
   source->failStreak = 0;

   int offset = source->firstDone ? firstMoofOffset(source->fetchBuffer, length) : 0;
   lock(&source->lock);
   source->firstDone = 1;
   appendWindow(source, source->fetchBuffer + offset, length - offset);
   unlock(&source->lock);
   source->nextSq++;
   return 0;
}

static void prefetchThread(uint64_t arg)
{
   LiveSource *source = (LiveSource *)(uintptr_t)arg;
   while (source->running && !source->cancel) {
      lock(&source->lock);
      uint64_t ahead = source->windowStart + source->windowLen > source->position ? source->windowStart + source->windowLen - source->position : 0;
      int primed = source->firstDone;
      unlock(&source->lock);

      if (primed && ahead >= TARGET_AHEAD) { sleepMs(PREFETCH_IDLE_MS); continue; }

      int rc = fetchNextSegment(source);
      if (rc == 1) sleepMs(EDGE_POLL_MS);        // at the edge, wait then retry the same sq (it's live, more is coming)
      else if (rc < 0) sleepMs(RECONNECT_MS);    // transient failure: back off and retry the same sq
   }
   exitThread();
}

LiveSource *openLiveSource(const char *baseUrl, long startSq, long edgeSq)
{
   LiveSource *source = (LiveSource *)calloc(1, sizeof *source);
   if (!source) return NULL;
   source->window = (uint8_t *)malloc(WINDOW_CAP);
   source->fetchBuffer = (uint8_t *)malloc(SEGMENT_FETCH_CAP);
   if (!source->window || !source->fetchBuffer) { closeLiveSource(source); return NULL; }

   strCopy(source->base, sizeof source->base, baseUrl);
   long start = edgeSq - START_BEHIND_EDGE;
   source->nextSq = start > startSq ? start : startSq;
   createLock(&source->lock);
   source->lockReady = 1;
   logInfo("[live] open sq %ld..%ld, starting at %ld\n", startSq, edgeSq, source->nextSq);

   source->running = 1;
   source->threadActive = spawnJoinableThread(&source->prefetchThread, prefetchThread, (uint64_t)(uintptr_t)source,
                          THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "live-fetch") == 0;
   if (!source->threadActive) { closeLiveSource(source); return NULL; }
   return source;
}

int64_t readLiveSource(LiveSource *source, void *buffer, uint64_t length)
{
   for (;;) {
      if (source->cancel) return 0;
      lock(&source->lock);
      if (source->position < source->windowStart) { unlock(&source->lock); return -1; }   // seeked before dropped history
      uint64_t available = source->windowStart + source->windowLen > source->position ? source->windowStart + source->windowLen - source->position : 0;
      if (available > 0) {
         uint64_t take = length < available ? length : available;
         memcpy(buffer, source->window + (source->position - source->windowStart), take);
         source->position += take;
         unlock(&source->lock);
         return (int64_t)take;
      }
      unlock(&source->lock);
      sleepMs(READ_WAIT_MS);   // nothing buffered at the read position yet; let the prefetch catch up
   }
}

int seekLiveSource(LiveSource *source, uint64_t offset)
{
   lock(&source->lock);
   int ok = offset >= source->windowStart;   // that far back is no longer buffered
   if (ok) source->position = offset;
   unlock(&source->lock);
   return ok ? 0 : -1;
}

uint64_t getLiveSourcePosition(const LiveSource *source) { return source->position; }

void cancelLiveSource(LiveSource *source) { if (source) source->cancel = 1; }

void closeLiveSource(LiveSource *source)
{
   if (!source) return;
   source->running = 0;
   source->cancel = 1;
   if (source->threadActive) { joinThread(source->prefetchThread); source->threadActive = 0; }
   if (source->lockReady) { destroyLock(&source->lock); source->lockReady = 0; }
   free(source->window);
   free(source->fetchBuffer);
   free(source);
}
