// direct-https - the byte channel simple-lib-https runs on when there is no tunnel: the console's
// own sockets, with our own TLS on top of them rather than the console's.

#include "direct-https.h"

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
#include "http.h"
#include "tls-transport.h"

#define TAG "[swarm] "

#define CONNECT_TIMEOUT_MS 10000
#define READ_TIMEOUT_MS    15000
#define STEP_MS               20

// nothing here may wait in the kernel: this all runs on the thread that also keeps the downloads and
// the tunnel moving, so a site that accepts a connection and then says nothing would stop everything
static int setNotWaiting(int handle)
{
   int on = 1;
   return setsockopt(handle, SOL_SOCKET, SO_NBIO, &on, sizeof on);
}

// whether the connection has finished being made: asking for room to write is how a socket says so
static int isConnected(int handle)
{
   fd_set writable;
   struct timeval now = { 0, 0 };

   FD_ZERO(&writable);
   FD_SET(handle, &writable);
   if (socketselect(handle + 1, NULL, &writable, NULL, &now) <= 0) return 0;

   int error = 0;
   socklen_t length = sizeof error;
   return getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
}

static int openDirect(const char *host, int port)
{
   struct hostent *found = gethostbyname(host);
   if (!found || !found->h_addr_list || !found->h_addr_list[0]) {
      logTrace(TAG "https: %s could not be looked up\n", host);
      return -1;
   }

   int handle = socket(AF_INET, SOCK_STREAM, 0);
   if (handle < 0) return -1;

   setNotWaiting(handle);

   struct sockaddr_in to;
   memset(&to, 0, sizeof to);
   to.sin_family = AF_INET;
   to.sin_port = htons((uint16_t)port);
   memcpy(&to.sin_addr.s_addr, found->h_addr_list[0], sizeof to.sin_addr.s_addr);

   if (connect(handle, (struct sockaddr *)&to, sizeof to) < 0 && sys_net_errno != SYS_NET_EINPROGRESS &&
       sys_net_errno != SYS_NET_EALREADY) {
      logTrace(TAG "https: %s did not accept a connection\n", host);
      socketclose(handle);
      return -1;
   }

   for (int waited = 0; waited < CONNECT_TIMEOUT_MS; waited += STEP_MS) {
      if (isConnected(handle)) return handle;
      sys_timer_usleep(STEP_MS * 1000);
   }

   logTrace(TAG "https: %s did not answer within %dms\n", host, CONNECT_TIMEOUT_MS);
   socketclose(handle);
   return -1;
}

// TLS reads a return of 0 as the connection ending, so waiting for data is this function's job
static int readDirect(int handle, void *buffer, int length)
{
   for (int waited = 0; waited < READ_TIMEOUT_MS; waited += STEP_MS) {
      int taken = recv(handle, buffer, length, 0);
      if (taken > 0) return taken;
      if (taken == 0) return -1;
      if (sys_net_errno != SYS_NET_EWOULDBLOCK && sys_net_errno != SYS_NET_EAGAIN) return -1;

      sys_timer_usleep(STEP_MS * 1000);
   }

   logWarn(TAG "https: nothing arrived within %dms\n", READ_TIMEOUT_MS);
   return -1;
}

// the socket does not wait any more, so a send that finds no room is retried rather than failed
static int writeDirect(int handle, const void *data, int length)
{
   for (int waited = 0; waited < READ_TIMEOUT_MS; waited += STEP_MS) {
      int sent = send(handle, data, length, 0);
      if (sent >= 0) return sent;
      if (sys_net_errno != SYS_NET_EWOULDBLOCK && sys_net_errno != SYS_NET_EAGAIN) return -1;

      sys_timer_usleep(STEP_MS * 1000);
   }

   return -1;
}

static void closeDirect(int handle)
{
   if (handle >= 0) socketclose(handle);
}

void useConsoleForHttps(void)
{
   static const TlsChannel directChannel = { openDirect, readDirect, writeDirect, closeDirect };

   bindTlsChannel(&directChannel);
   initModernHttp();   // our own TLS either way, so a site behaves the same with or without the vpn
   logWarn(TAG "https: requests will run over the console's own connection\n");
}
