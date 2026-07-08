#pragma once

// http-fetch - one blocking HTTPS request over the PS3 system stack
// (cellSsl/cellHttps). Source-agnostic: extractors use this to talk to any
// site. Enforces certificate verification against the console's built-in CA
// set (proven to trust modern sites). Response body is written NUL-terminated
// into out (truncated to outCap-1). Returns 0 on success (HTTP status in
// *statusOut), negative on transport/SSL error.
//
// Requires CELL_SYSMODULE_HTTPS to be loaded already (main loads it once).

#include <cell/http.h>

int httpFetch(const char *method, const char *url,
              const CellHttpHeader *headers, int headerCount,
              const void *body, int bodyLen,
              char *out, int outCap, int *outLen, int *statusOut);
