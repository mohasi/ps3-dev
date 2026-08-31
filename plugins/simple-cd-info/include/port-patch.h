#pragma once
#include <stdint.h>

// Move the CD lookup off port 80. The firmware module that talks to the AMG server
// (x3_amgsdk) has the port as a constant in its own code; this rewrites it so the
// lookup connects to our listener on `port` instead, leaving port 80 free for webMAN.
// Safe to call repeatedly: it only writes when the constant is still 80.
// Returns the number of sites patched, 0 if they already carry our port, or one of:
#define AMG_MODULE_NOT_LOADED   -1
#define AMG_PORT_NOT_FOUND      -2   // module loaded, but no port constant we recognise
#define AMG_HOST_NOT_FOUND      -3   // module loaded, but the AMG host string is not there

int patchAmgLookupPort(uint16_t port);

// Redirect the dead AMG host to loopback by overwriting its hostname string in
// x3_amgsdk's rodata with 127.0.0.1, so the lookup resolves to our listener with
// no gethostbyname hook. A data write, safe on live vsh under HEN (unlike a code
// detour). Returns sites patched, 0 if already loopback, or a code above.
int patchAmgHost(void);
