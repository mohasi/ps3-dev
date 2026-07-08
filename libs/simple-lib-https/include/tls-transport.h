#pragma once

// tls-transport - the BearSSL HTTPS plumbing behind the modern http transport (transport-bearssl). One
// connection = one TCP socket + one BearSSL client engine; a request runs on it, then the response head is
// parsed and the body is read incrementally. HTTP/1.1 + "Connection: close" (or HTTP/1.0) means the body
// is delimited by
// Content-Length or by the peer closing - there is no chunked decoding to get wrong for the hosts we talk
// to (googlevideo answers a Range request with 206 + Content-Length).

#include <stdint.h>

typedef struct TlsConn TlsConn;

// create the resolver lock that serialises gethostbyname across worker threads. call once at startup
// (from the transport's init) before any connection is opened.
void initTlsResolver(void);

// resolve host and run a full TLS 1.2 client handshake (cert + hostname validated against the bundled
// roots, SNI set) to host:443. returns the connection, or NULL on resolve/connect/handshake failure.
TlsConn *openTlsConn(const char *host);

// send one request on conn. httpVersion is "HTTP/1.0" or "HTTP/1.1". keepAlive asks the server to hold the
// connection open for reuse (Connection: keep-alive vs close). extraHeaders, if non-NULL, is a block of
// already-formatted "Name: value\r\n" lines (e.g. Range, User-Agent). body/bodyLen are optional. the first
// write drives the handshake to completion. 0 / -1.
int sendTlsRequest(TlsConn *conn, const char *method, const char *httpVersion, int keepAlive, const char *host,
                   const char *path, const char *extraHeaders, const void *body, int bodyLen);

const char *getTlsHost(TlsConn *conn);   // the host this connection is to (pool key)
int         isTlsReusable(TlsConn *conn); // 1 if the whole body was read and the server left it open for reuse

// read and parse the response head. fills *status (HTTP status code) and *totalSize (the full resource
// size from Content-Range, else Content-Length, else 0). retains any body bytes read past the head for
// the first recvTls. 0 / -1.
int readTlsHead(TlsConn *conn, int *status, uint64_t *totalSize);

// copy a response header's value (case-insensitive name, e.g. "Location") into out, NUL-terminated.
// valid after readTlsHead. returns 0 if the header was present, -1 if not.
int getTlsHeader(TlsConn *conn, const char *name, char *out, int cap);

// read up to cap body bytes into buffer. returns >0 bytes, 0 at end of body, -1 on transport/TLS error.
int64_t recvTls(TlsConn *conn, void *buffer, uint64_t cap);

// the last BearSSL engine error (BR_ERR_*), for diagnostics on a failed request. 0 = BR_ERR_OK.
int getLastTlsError(TlsConn *conn);

void closeTlsConn(TlsConn *conn);

// split "https://host/path" into a bare host (into hostOut, capped) and a path pointer into the url
// (defaults to "/"). 0 / -1 (not https, or host empty / too long).
int splitHttpsUrl(const char *url, char *hostOut, int hostCap, const char **pathOut);
