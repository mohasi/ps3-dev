// tunnel-torrent - the calls simple-lib-torrent makes for a network, answered by the WireGuard
// tunnel. Every one of them is a thin wrapper: the library holds no idea of how traffic gets out.

#include "tunnel-torrent.h"

#include <sys/sys_time.h>   // sys_time_get_system_time, the clock the library measures with

#include "dbg.h"
#include "torrent-net.h"
#include "wg-net.h"
#include "wg-random.h"

#define TAG "[swarm] "

static int resolveThroughTunnel(const char *hostName, uint32_t *address, int timeoutMs)
{
   return resolveWgHost(hostName, address, timeoutMs);
}

static int openUdpThroughTunnel(void)
{
   return openWgUdp(0);   // any free port
}

static int sendUdpThroughTunnel(int handle, uint32_t address, uint16_t port, const void *data, int length)
{
   WgEndpoint to = { address, port };
   return sendWgTo(handle, &to, data, length);
}

static int receiveUdpThroughTunnel(int handle, uint32_t *fromAddress, uint16_t *fromPort, void *buffer, int capacity)
{
   WgEndpoint from = { 0, 0 };
   int length = recvWgFrom(handle, &from, buffer, capacity);

   *fromAddress = from.address;
   *fromPort = from.port;
   return length;
}

static void closeUdpThroughTunnel(int handle)
{
   closeWgUdp(handle);
}

static int openTcpThroughTunnel(uint32_t address, uint16_t port)
{
   return openWgTcp(address, port);
}

static int sendTcpThroughTunnel(int handle, const void *data, int length, int timeoutMs)
{
   return sendWgTcp(handle, data, length, timeoutMs);
}

static int receiveTcpThroughTunnel(int handle, void *buffer, int capacity)
{
   return recvWgTcp(handle, buffer, capacity);
}

static void closeTcpThroughTunnel(int handle)
{
   closeWgTcp(handle);
}

static uint64_t getNowMs(void)
{
   return sys_time_get_system_time() / 1000;   // the system clock counts microseconds since boot
}

static void serviceTunnel(int waitMs)
{
   serviceWgNetwork(waitMs);
}

void useTunnelForTorrents(void)
{
   static const TorrentNetwork tunnelNetwork = { resolveThroughTunnel, openUdpThroughTunnel, sendUdpThroughTunnel,
                                                 receiveUdpThroughTunnel, closeUdpThroughTunnel,
                                                 openTcpThroughTunnel, isWgTcpConnecting, isWgTcpFailed,
                                                 sendTcpThroughTunnel, receiveTcpThroughTunnel,
                                                 closeTcpThroughTunnel, serviceTunnel, getRandomBytes, getNowMs };

   bindTorrentNetwork(&tunnelNetwork);
   logInfo(TAG "torrents: trackers and peers will be reached over the tunnel\n");
}
