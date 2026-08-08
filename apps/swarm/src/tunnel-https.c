// tunnel-https - the byte channel that puts simple-lib-https on the WireGuard tunnel.
//
// Four small functions, because that is the whole of what TLS needs underneath it: open a
// connection to a named host, read, write, close.

#include "tunnel-https.h"

#include "dbg.h"
#include "http.h"
#include "tls-transport.h"
#include "wg-net.h"

#define TAG "[swarm] "

#define RESOLVE_TIMEOUT_MS 8000
#define CONNECT_TIMEOUT_MS 8000
#define READ_TIMEOUT_MS   15000   // matches what the socket channel allows a stalled read
#define SEND_TIMEOUT_MS    8000
#define STEP_MS              20

// the name is looked up through the tunnel as well: the console's own resolver would ask our
// provider's name server directly, which tells them every host we visit
static int openThroughTunnel(const char *host, int port)
{
   uint32_t address = 0;
   if (resolveWgHost(host, &address, RESOLVE_TIMEOUT_MS) != 0) return -1;

   return connectWgTcp(address, (uint16_t)port, CONNECT_TIMEOUT_MS);
}

// TLS reads a return of 0 as the connection ending, so waiting for data is this function's job
static int readThroughTunnel(int handle, void *buffer, int length)
{
   for (int waited = 0; waited < READ_TIMEOUT_MS; waited += STEP_MS) {
      int taken = recvWgTcp(handle, buffer, length);
      if (taken != 0) return taken;
      if (isWgTcpFinished(handle)) return -1;
      if (serviceWgNetwork(STEP_MS) != 0) return -1;
   }

   logWarn(TAG "https: nothing arrived within %dms\n", READ_TIMEOUT_MS);
   return -1;
}

static int writeThroughTunnel(int handle, const void *data, int length)
{
   return sendWgTcp(handle, data, length, SEND_TIMEOUT_MS);
}

static void closeThroughTunnel(int handle)
{
   closeWgTcp(handle);
}

void useTunnelForHttps(void)
{
   static const TlsChannel tunnelChannel = { openThroughTunnel, readThroughTunnel, writeThroughTunnel,
                                             closeThroughTunnel };

   bindTlsChannel(&tunnelChannel);
   initModernHttp();   // BearSSL, so nothing reaches the console's own TLS or its sockets
   logInfo(TAG "https: requests will run over the tunnel\n");
}
