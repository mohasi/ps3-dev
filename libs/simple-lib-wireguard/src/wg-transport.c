#include "wg-transport.h"

#include <arpa/inet.h>
#include <netex/errno.h>
#include <netinet/in.h>
#include <netex/net.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "dbg.h"
#include "string-utilities.h"

#define TAG "[wg] "

// Room for a burst of packets to sit in while the app is busy with something else. Without this
// the console's default is a few packets deep, and a fast sender overruns it between service calls,
// which shows up as loss the stream then has to repair.
#define SOCKET_BUFFER_BYTES 262144

static void growSocketBuffers(int socketHandle)
{
   int size = SOCKET_BUFFER_BYTES;
   if (setsockopt(socketHandle, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) < 0)
      logWarn(TAG "transport: could not grow the receive buffer, errno=%d\n", sys_net_errno);

   size = SOCKET_BUFFER_BYTES;
   if (setsockopt(socketHandle, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) < 0)
      logWarn(TAG "transport: could not grow the send buffer, errno=%d\n", sys_net_errno);

   int actual = 0;
   socklen_t actualLength = sizeof actual;
   if (getsockopt(socketHandle, SOL_SOCKET, SO_RCVBUF, &actual, &actualLength) == 0)
      logInfo(TAG "transport: receive buffer is %d bytes\n", actual);
}

int openWgTransport(WgTransport *transport, uint32_t address, uint16_t port)
{
   transport->socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
   if (transport->socketHandle < 0) {
      logError(TAG "transport: socket failed, errno=%d\n", sys_net_errno);
      return -1;
   }
   transport->timeoutMs = -1;   // nothing set yet, so the first receive always sets one
   growSocketBuffers(transport->socketHandle);

   // connecting a UDP socket sends nothing. it fixes the destination, so this socket can only
   // ever reach the VPN server, and it lets us use plain send and recv.
   struct sockaddr_in server;
   memSet(&server, 0, sizeof server);
   server.sin_family = AF_INET;
   server.sin_port = htons(port);
   server.sin_addr.s_addr = htonl(address);

   if (connect(transport->socketHandle, (struct sockaddr *)&server, sizeof server) < 0) {
      logError(TAG "transport: connect failed, errno=%d\n", sys_net_errno);
      socketclose(transport->socketHandle);
      transport->socketHandle = -1;
      return -1;
   }

   return 0;
}

int sendWgDatagram(WgTransport *transport, const void *data, int length)
{
   int sent = send(transport->socketHandle, data, length, 0);
   if (sent != length) {
      logError(TAG "transport: send sent %d of %d bytes, errno=%d\n", sent, length, sys_net_errno);
      return -1;
   }
   return sent;
}

int receiveWgDatagram(WgTransport *transport, void *buffer, int capacity, int timeoutMs)
{
   // a wait of zero would mean "wait for ever" to the socket, so the shortest real wait stands in
   // for it. changing the wait costs a call into the network stack, so it is only changed when it
   // differs from what the socket is already set to.
   if (timeoutMs < 1) timeoutMs = 1;
   if (timeoutMs != transport->timeoutMs) {
      struct timeval timeout;
      timeout.tv_sec = timeoutMs / 1000;
      timeout.tv_usec = (timeoutMs % 1000) * 1000;
      setsockopt(transport->socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
      transport->timeoutMs = timeoutMs;
   }

   int received = recv(transport->socketHandle, buffer, capacity, 0);
   if (received > 0) return received;

   // a timeout is not a failure: the caller decides whether to wait again or give up
   int error = sys_net_errno;
   if (received == 0 || error == SYS_NET_EAGAIN || error == SYS_NET_ETIMEDOUT) return 0;

   logError(TAG "transport: recv failed, errno=%d\n", error);
   return -1;
}

void closeWgTransport(WgTransport *transport)
{
   if (transport->socketHandle >= 0) socketclose(transport->socketHandle);
   transport->socketHandle = -1;
}
