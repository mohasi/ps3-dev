#pragma once

// The console's own sockets as the byte channel simple-lib-https runs on, for when there is no
// tunnel. Our own TLS still does the encryption.

void useConsoleForHttps(void);
