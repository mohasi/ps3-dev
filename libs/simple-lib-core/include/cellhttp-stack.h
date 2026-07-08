#pragma once

// cellhttp-stack - one-time bringup of the firmware http/ssl/https stack, used by the cellHttp transport
// (transport-cellhttp). Cycling init/end faults after a stream is torn down mid-download, so the stack
// comes up once on first use and stays resident for the whole app run.
//
// Consumers link -lhttp_stub -lhttp_util_stub -lssl_stub -lnetctl_stub and load CELL_SYSMODULE_HTTPS first.

#include <cell/http.h>
#include <cell/ssl.h>

int  ensureHttpStack(void);   // bring the stack up once (idempotent); 0 / -1.
void termHttpStack(void);     // tear it down; app exit only, not between requests.

// certificate verify callback for cellHttp clients - enforces validation against the console CA store
// (media/api hosts chain to CAs already there). a non-zero return fails the handshake.
int32_t verifyCellHttpsCert(uint32_t verifyErr, CellSslCert const cert[], int certNum, const char *hostname, CellHttpSslId id, void *arg);
