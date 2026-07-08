#pragma once

// http-fs - a read-only VFS backend that streams http(s):// urls by HTTP range
// requests. Once registered, any consumer can openFs / readFs / seekFs a url
// exactly like a local file - the bytes are fetched on demand, never downloaded
// in full - so e.g. the video player's demuxer plays straight from a url.
//
// Opt-in like the NTFS/exFAT backends: call initHttpFs() once, after the net is
// up and CELL_SYSMODULE_HTTPS is loaded. Only binaries that call it pull this TU
// (and thus libhttp/libssl); plugins that never touch urls stay lean.
//
// Consumers must link -lhttp_stub -lhttp_util_stub -lssl_stub -lnetctl_stub.

// register the http(s):// route. call once at startup (mirrors initExfat/initNtfs).
void initHttpFs(void);

// unregister the route (new url opens fail). does not affect already-open streams.
void termHttpFs(void);

// brings libhttp/libssl/libhttps up once and keeps them resident for the app run (0 / -1). Both this
// backend and any per-call HTTPS helper (e.g. yo-player's http-fetch) share it, so the stack is never
// re-initialized between requests - cycling init/end faults after a stream is torn down mid-download.
int  ensureHttpStack(void);

// tears the shared stack down. Call once at app exit (not between requests).
void termHttpStack(void);
