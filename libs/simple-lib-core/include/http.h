#pragma once

// http - a transport-agnostic HTTP(S) client for the PS3.
//
// An app picks ONE transport at startup and every call below runs over it:
//   initSystemHttp();   // cellHttp - firmware TLS, free weight, reaches RSA hosts (Google/YouTube)
//   initModernHttp();   // BearSSL  - modern TLS (~80 KB), also reaches ECDSA-only hosts (Cloudflare)
// initModernHttp() overrides initSystemHttp() if both are called. A binary that calls neither pulls no
// transport (and no TLS code) at all. The system transport lives in simple-lib-core; the modern one in
// simple-lib-https (only linked binaries that call initModernHttp pull BearSSL).
//
// The API is the same either way - only which hosts the handshake can reach differs by transport.

#include <stdint.h>

typedef struct { const char *name; const char *value; } HttpHeader;

// section: one-shot request/response

// perform one request and write the response BODY (NUL-terminated, truncated to cap-1) into out. method
// is "GET"/"POST"/... returns 0 when a response arrived (HTTP status in *status, body length in *outLen),
// negative on a connect/transport/TLS failure. a non-2xx status is still a 0 return - inspect *status.
int fetchHttp(const char *method, const char *url, const HttpHeader *headers, int headerCount,
              const void *body, int bodyLen, char *out, int cap, int *outLen, int *status);

// one response header to copy out of the reply, e.g. { "Location", buffer, sizeof buffer }. value is
// emptied when the header is absent (or the transport doesn't keep the response head).
typedef struct { const char *name; char *value; int cap; } HttpHeaderCapture;

// as fetchHttp, plus one captured response header. fetchHttp is this with capture = NULL.
int fetchHttpCapturing(const char *method, const char *url, const HttpHeader *headers, int headerCount,
                       const void *body, int bodyLen, char *out, int cap, int *outLen, int *status,
                       HttpHeaderCapture *capture);

// convenience: GET url with just a default User-Agent.
int getHttp(const char *url, char *out, int cap, int *outLen, int *status);

// section: streaming (media playback)

// a seekable read over an http(s):// url: bytes are fetched on demand by range requests and buffered by a
// ring + prefetch thread, never downloaded in full. a seek off the buffered window issues a fresh range
// request. read-only.
typedef struct HttpStream HttpStream;

HttpStream *openHttpStream(const char *url);                                    // NULL on failure
int64_t     readHttpStream(HttpStream *stream, void *buffer, uint64_t length);  // bytes read, 0 eof, -1 err
int         seekHttpStream(HttpStream *stream, uint64_t offset);                // absolute seek; 0 / -1
uint64_t    getHttpStreamSize(HttpStream *stream);                             // total size (0 if unknown)
void        closeHttpStream(HttpStream *stream);

// section: setup

int  isHttpUrl(const char *path);   // true for an http:// or https:// url
void initSystemHttp(void);          // register the cellHttp transport (simple-lib-core)
void initModernHttp(void);          // register the BearSSL transport (simple-lib-https); overrides system
void shutdownHttp(void);            // release the transport's idle pooled connections (call at app exit)
