# simple-lib-https

Self-contained modern TLS for the PS3 — the HTTPS the console's own firmware cannot do. It plugs in
as an alternative transport behind the shared http client (`http.h` in simple-lib-core), so an app
that opts in gets every `fetchHttp` / `getHttp` / `openHttpStream` call routed over BearSSL instead
of the firmware stack.

## Why it exists

The PS3's system TLS (`cellHttp` / `cellSsl`, an OpenSSL 0.9.8-era library) only offers
**RSA-authenticated** cipher suites. It cannot handshake with a host that presents an **ECDSA**
certificate — now the default for Cloudflare's Free plan and many CDNs. The connection fails during
cipher negotiation, before any certificate is even seen, with `0x80710A06`
(`CELL_HTTPS_ERROR_HANDSHAKE`). Google/YouTube still work because Google serves a dual RSA+ECDSA
cert and hands the PS3 the RSA one; a host that serves ECDSA-only does not.

This library sidesteps the firmware entirely: it links [BearSSL](https://bearssl.org) and drives a
raw network socket itself, so it can negotiate ECDHE-ECDSA / ECDHE-RSA, TLS 1.2, with SNI — exactly
what those hosts require.

## Guarantees

- **Real certificate checking.** The server's certificate chain is verified against a bundled set of
  current root authorities, and the hostname is matched. There is no verification-off mode.
- **No leaks.** BearSSL does zero dynamic allocation; the per-connection working buffer and socket
  are released on close.

## How to use it

The API is the shared http client (`simple-lib-core/include/http.h`) — the same
`fetchHttp` / `getHttp` / `openHttpStream` calls regardless of which transport is underneath. This
library only changes *which* transport those calls run over. An app opts in once at startup:

```c
initModernHttp();   // every http request + media stream now runs over BearSSL
...
shutdownHttp();     // at exit: drops the idle keep-alive connection pool
```

`initModernHttp()` **overrides** the free firmware backend (`initSystemHttp()` / cellHttp), so an
app that opts in also reaches ECDSA-only hosts. It adds roughly 80 KB, so it is opt-in and
apps-only — a plugin that wants http keeps the free cellHttp backend and never links this.

One-shot requests (thumbnails, API calls) use HTTP/1.0 keep-alive over a host-keyed connection pool,
so a screen of thumbnails costs a handful of TLS handshakes instead of one per image. Media streams
open a fresh HTTP/1.1 connection and follow redirects.

## Layout

- `bearssl/` — BearSSL, upstream `src/` + `inc/`, kept as-is except for edits tagged `// [ps3]`
  (grep for them before updating BearSSL, to reapply). There is currently one: an explicit cast in
  `bearssl_ssl.h` to silence a spurious old-GCC pointer warning under the repo's `-Werror`.
- `src/tls-transport.c` — the BearSSL plumbing: socket transport, handshake, request, response-head
  parse, incremental body read, per-connection reuse.
- `src/transport-bearssl.c` — the `HttpTransport` (open / read / close / shutdown) plus the
  keep-alive connection pool, redirect following, and `initModernHttp`.
- `src/trust-anchors.c` — the bundled root authorities as BearSSL structs (generated from root PEMs).
- `include/tls-transport.h` — the plumbing API shared between the two source files above
  (`openTlsConn`, `sendTlsRequest`, `readTlsHead`, `recvTls`, connection-reuse helpers, …).

The linker pulls only the BearSSL object files a TLS 1.2 client actually needs; the rest of the
vendored tree never reaches the binary.

## Trust anchors

`trust-anchors.c` is generated. To add or refresh a root, drop its PEM in and regenerate the BearSSL
`br_x509_trust_anchor` structs (the generator reads the subject name and public key out of the
cert). The current set: **GTS Root R1** (RSA), **GTS Root R4** (EC P-384), **ISRG Root X1** (RSA),
**ISRG Root X2** (EC P-384) — i.e. Google Trust Services and Let's Encrypt, both RSA and ECDSA.
