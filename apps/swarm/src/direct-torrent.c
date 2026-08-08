// direct-torrent - the calls simple-lib-torrent makes for a network, answered by the console's own
// connection rather than the tunnel. Used when the vpn is off, or when it is down and the kill
// switch allows it. Everything here leaves the console under its real address.

#include "direct-torrent.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netex/errno.h>
#include <netex/net.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sys_time.h>
#include <sys/time.h>     // before select.h, which declares socketselect in terms of its timeval
#include <sys/select.h>
#include <sys/timer.h>

#include "dbg.h"
#include "torrent-net.h"
#include "wg-random.h"   // the same random source the tunnel uses, which is the console's own

#define TAG "[swarm] "

static int setNotWaiting(int handle)
{
   int on = 1;
   return setsockopt(handle, SOL_SOCKET, SO_NBIO, &on, sizeof on);
}

static int resolveDirect(const char *hostName, uint32_t *address, int timeoutMs)
{
   (void)timeoutMs;   // the console's resolver has a timeout of its own

   struct hostent *host = gethostbyname(hostName);
   if (!host || !host->h_addr_list || !host->h_addr_list[0]) {
      logTrace(TAG "direct: %s could not be looked up\n", hostName);
      return -1;
   }

   uint32_t found;
   memcpy(&found, host->h_addr_list[0], sizeof found);
   *address = ntohl(found);
   return 0;
}

static int openUdpDirect(void)
{
   int handle = socket(AF_INET, SOCK_DGRAM, 0);
   if (handle < 0) return -1;

   setNotWaiting(handle);
   return handle;
}

static int sendUdpDirect(int handle, uint32_t address, uint16_t port, const void *data, int length)
{
   struct sockaddr_in to;
   memset(&to, 0, sizeof to);
   to.sin_family = AF_INET;
   to.sin_addr.s_addr = htonl(address);
   to.sin_port = htons(port);

   int sent = sendto(handle, data, length, 0, (struct sockaddr *)&to, sizeof to);
   if (sent == length) return 0;   // the library reads 0 as sent, not the byte count

   logTrace(TAG "direct: %d of %d bytes went to port %d, errno %d\n", sent, length, port, sys_net_errno);
   return -1;
}

static int receiveUdpDirect(int handle, uint32_t *fromAddress, uint16_t *fromPort, void *buffer, int capacity)
{
   struct sockaddr_in from;
   socklen_t fromLength = sizeof from;
   memset(&from, 0, sizeof from);

   int length = recvfrom(handle, buffer, capacity, 0, (struct sockaddr *)&from, &fromLength);
   if (length <= 0) {
      // temporary, while the tracker's silence on this path is worked out
      if (length < 0 && sys_net_errno != SYS_NET_EWOULDBLOCK && sys_net_errno != SYS_NET_EAGAIN)
         logTrace(TAG "direct: reading a datagram failed, errno %d\n", sys_net_errno);

      return 0;   // nothing waiting, which is not a failure
   }

   *fromAddress = ntohl(from.sin_addr.s_addr);
   *fromPort = ntohs(from.sin_port);
   return length;
}

static void closeUdpDirect(int handle)
{
   if (handle >= 0) socketclose(handle);
}

static int openTcpDirect(uint32_t address, uint16_t port)
{
   int handle = socket(AF_INET, SOCK_STREAM, 0);
   if (handle < 0) return -1;

   setNotWaiting(handle);

   struct sockaddr_in to;
   memset(&to, 0, sizeof to);
   to.sin_family = AF_INET;
   to.sin_addr.s_addr = htonl(address);
   to.sin_port = htons(port);

   // it will not have finished by the time this returns, which is what isTcpConnecting is for
   if (connect(handle, (struct sockaddr *)&to, sizeof to) < 0 && sys_net_errno != SYS_NET_EINPROGRESS &&
       sys_net_errno != SYS_NET_EALREADY) {
      socketclose(handle);
      return -1;
   }

   return handle;
}

// whether the connection has finished being made: asking for room to write is how a socket says so
static int isConnectionSettled(int handle, int *failed)
{
   fd_set writable, broken;
   struct timeval now = { 0, 0 };

   FD_ZERO(&writable);
   FD_ZERO(&broken);
   FD_SET(handle, &writable);
   FD_SET(handle, &broken);

   *failed = 0;
   if (socketselect(handle + 1, NULL, &writable, &broken, &now) <= 0) return 0;
   if (FD_ISSET(handle, &broken)) { *failed = 1; return 1; }

   int error = 0;
   socklen_t length = sizeof error;
   if (getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error != 0) *failed = 1;

   return 1;
}

static int isTcpConnectingDirect(int handle)
{
   int failed = 0;
   return handle >= 0 && !isConnectionSettled(handle, &failed);
}

static int isTcpFailedDirect(int handle)
{
   if (handle < 0) return 1;

   int failed = 0;
   isConnectionSettled(handle, &failed);
   return failed;
}

static int sendTcpDirect(int handle, const void *data, int length, int timeoutMs)
{
   (void)timeoutMs;   // the caller retries; nothing here waits

   int sent = send(handle, data, length, 0);
   return sent < 0 ? -1 : sent;
}

static int receiveTcpDirect(int handle, void *buffer, int capacity)
{
   int taken = recv(handle, buffer, capacity, 0);
   if (taken > 0) return taken;
   if (taken == 0) return -1;   // the other end has closed

   return sys_net_errno == SYS_NET_EWOULDBLOCK || sys_net_errno == SYS_NET_EAGAIN ? 0 : -1;
}

static void closeTcpDirect(int handle)
{
   if (handle >= 0) socketclose(handle);
}

// the tunnel needs servicing to move packets along; the console's own sockets do not
static void serviceDirect(int waitMs)
{
   if (waitMs > 0) sys_timer_usleep((useconds_t)waitMs * 1000);
}

static uint64_t getNowMs(void)
{
   return sys_time_get_system_time() / 1000;
}

void useConsoleForTorrents(void)
{
   static const TorrentNetwork directNetwork = { resolveDirect,        openUdpDirect,        sendUdpDirect,
                                                 receiveUdpDirect,     closeUdpDirect,       openTcpDirect,
                                                 isTcpConnectingDirect, isTcpFailedDirect,   sendTcpDirect,
                                                 receiveTcpDirect,     closeTcpDirect,       serviceDirect,
                                                 getRandomBytes,       getNowMs };

   bindTorrentNetwork(&directNetwork);
   logWarn(TAG "torrents: trackers and peers will be reached over the console's own connection\n");
}
