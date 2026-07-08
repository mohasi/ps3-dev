#pragma once

// http-transport - the pluggable transport behind the http module (see http.h).
//
// http.c owns everything transport-independent: the one-shot read loop, and the streaming ring buffer +
// prefetch thread. A transport owns one connection's worth of "start a request" and "read more bytes".
// Two transports implement it: cellHttp (system TLS, simple-lib-core) and BearSSL (modern TLS,
// simple-lib-https). The same http.h API runs over whichever one an app binds.

#include "http.h"   // HttpHeader

typedef struct {
   // start a request and read its response head. method is "GET"/"POST"/... headers/body are optional.
   // returns an opaque per-request handle, or NULL on a connect/transport/TLS failure. reports the HTTP
   // status in *status (a non-2xx is NOT a failure - the caller decides), and, when the caller asks,
   // the total resource size in *totalSize (from Content-Range/Content-Length, 0 if unknown).
   void *(*open)(const char *method, const char *url, const HttpHeader *headers, int headerCount,
                 const void *body, int bodyLen, int *status, uint64_t *totalSize);

   // read up to cap bytes of the body into buffer. returns:
   //   >0  bytes read
   //    0  end of body
   //   -1  the connection died - a streaming reader reconnects at the current offset
   //   -2  a transient stall (no data this turn, connection still alive) - retry
   int64_t (*read)(void *handle, void *buffer, uint64_t cap);

   void (*close)(void *handle);

   // release any idle pooled connections at app shutdown. optional - may be NULL.
   void (*shutdown)(void);
} HttpTransport;

// wire a transport into http.c. an `override` bind claims the slot permanently (modern wins over system
// regardless of init order); a non-override bind is ignored once overridden.
void bindHttpTransport(const HttpTransport *transport, int override);
