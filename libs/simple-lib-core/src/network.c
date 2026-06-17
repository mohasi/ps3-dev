// Shared networking helpers for simple-lib-core.
//
// getLocalIpv4 reports the console's own IPv4 address. The FTP listener binds to
// INADDR_ANY (all interfaces), so it deliberately does not own an address — the
// address is a property of the network, resolved here on demand for display.
// A failure return also doubles as a "no usable network" signal for callers.

#include "network.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netex/net.h>
#include <sys/socket.h>

#include "string-utilities.h"

int getLocalIpv4(uint32_t *ip)
{
   if (!ip) return -1;
   *ip = 0;

   int probe = socket(AF_INET, SOCK_DGRAM, 0);
   if (probe < 0) return -1;

   // "Connecting" a UDP socket sends nothing, but it makes the stack pick the
   // source interface for that destination; getsockname then reports its IP.
   // With no network configured the connect (or getsockname) fails, so the
   // caller can treat a negative return as "offline".
   struct sockaddr_in remote;
   memSet(&remote, 0, sizeof remote);
   remote.sin_family = AF_INET;
   remote.sin_port = htons(53);
   remote.sin_addr.s_addr = htonl(0x08080808);

   int result = -1;
   if (connect(probe, (struct sockaddr *)&remote, sizeof remote) == 0) {
     struct sockaddr_in local;
     socklen_t length = sizeof local;
     if (getsockname(probe, (struct sockaddr *)&local, &length) == 0 && local.sin_addr.s_addr != 0) {
       *ip = (uint32_t)local.sin_addr.s_addr;
       result = 0;
     }
   }

   socketclose(probe);
   return result;
}

int isNetworkAvailable(void)
{
   uint32_t ip;
   return getLocalIpv4(&ip) == 0;
}
