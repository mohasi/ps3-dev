#pragma once
#include <stdint.h>

// Move the CD lookup off port 80. The firmware module that talks to the AMG server
// (x3_amgsdk) has the port as a constant in its own code; this rewrites it so the
// lookup connects to our listener on `port` instead, leaving port 80 free for webMAN.
// Safe to call repeatedly: it only writes when the constant is still 80.
// Returns the number of sites patched, 0 if they already carry our port, or one of:
#define AMG_MODULE_NOT_LOADED   -1
#define AMG_PORT_NOT_FOUND      -2   // module loaded, but no port constant we recognise

int patchAmgLookupPort(uint16_t port);
