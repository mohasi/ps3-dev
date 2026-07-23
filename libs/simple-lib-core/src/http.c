// http - transport-agnostic HTTP client: transport binding + one-shot request/response (see http.h).
//
// This TU is the LIGHT half: binding a transport, the url predicate, and the buffered request/response
// helpers. It uses no malloc and spawns no threads, so a malloc-free VSH PRX can call isHttpUrl / fetchHttp
// without dragging in libc's heap. The seekable streaming engine (malloc + 4 MB ring + prefetch thread)
// lives in http-stream.c and is pulled only by a caller that references openHttpStream (see http-internal.h).

#include "http.h"
#include "http-internal.h"   // DEFAULT_UA, getActiveTransport
#include "dbg.h"

#include <string.h>

#define MAX_STALL_RETRIES 15   // consecutive transient stalls tolerated before a one-shot body is abandoned

static const HttpTransport *transport;
static int                 transportLatched;   // set once a modern (override) transport claimed the slot

void bindHttpTransport(const HttpTransport *ops, int override)
{
   if (transportLatched && !override) return;   // modern already won; ignore a later system bind
   transport = ops;
   if (override) transportLatched = 1;
}

const HttpTransport *getActiveTransport(void) { return transport; }

void shutdownHttp(void)
{
   if (transport && transport->shutdown) transport->shutdown();
}

int isHttpUrl(const char *path)
{
   return strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0;
}

// section: one-shot request/response

int fetchHttp(const char *method, const char *url, const HttpHeader *headers, int headerCount,
              const void *body, int bodyLen, char *out, int cap, int *outLen, int *status)
{
   return fetchHttpCapturing(method, url, headers, headerCount, body, bodyLen, out, cap, outLen, status, NULL);
}

int fetchHttpCapturing(const char *method, const char *url, const HttpHeader *headers, int headerCount,
                       const void *body, int bodyLen, char *out, int cap, int *outLen, int *status,
                       HttpHeaderCapture *capture)
{
   if (outLen) *outLen = 0;
   if (status) *status = 0;
   if (capture) capture->value[0] = '\0';
   if (!transport) { logError("[http] no transport bound\n"); return -1; }

   void *handle = transport->open(method, url, headers, headerCount, body, bodyLen, status, NULL);
   if (!handle) return -1;

   if (capture && transport->getResponseHeader)
      transport->getResponseHeader(handle, capture->name, capture->value, capture->cap);

   int written = 0, stalls = 0, failed = 0;
   while (cap > 0 && written < cap - 1) {
      int64_t got = transport->read(handle, out + written, (uint64_t)(cap - 1 - written));
      if (got == -2) { if (++stalls > MAX_STALL_RETRIES) { failed = 1; break; } continue; }   // gave up waiting: body incomplete
      if (got == -1) { failed = 1; break; }                                    // connection died mid-body
      if (got == 0) break;                                                     // clean end of body
      stalls = 0;
      written += (int)got;
   }
   if (cap > 0) out[written] = '\0';
   if (outLen) *outLen = written;

   transport->close(handle);
   // a transport death or a stall we gave up on means the body is truncated; the http.h contract promises a
   // negative return on transport failure so callers don't parse a partial body as a complete response.
   return failed ? -1 : 0;
}

int getHttp(const char *url, char *out, int cap, int *outLen, int *status)
{
   HttpHeader userAgent = { "User-Agent", DEFAULT_UA };
   return fetchHttp("GET", url, &userAgent, 1, NULL, 0, out, cap, outLen, status);
}
