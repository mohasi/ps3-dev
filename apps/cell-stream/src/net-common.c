#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/sys_time.h>

#include "dbg.h"
#include "net-common.h"

uint64_t getTimeUs(void)
{
   sys_time_sec_t seconds;
   sys_time_nsec_t nanoseconds;
   sys_time_get_current_time(&seconds, &nanoseconds);
   return (uint64_t)seconds * 1000000ull + nanoseconds / 1000;
}

void setReceiveTimeout(int socketValue, int milliseconds)
{
   struct timeval timeout;
   timeout.tv_sec = milliseconds / 1000;
   timeout.tv_usec = (milliseconds % 1000) * 1000;
   setsockopt(socketValue, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
}

void drainSocket(int socketValue)
{
   char discard[PACKET_MAX];
   setReceiveTimeout(socketValue, 50);
   while (recv(socketValue, discard, sizeof discard, 0) > 0) ;
}

int openClientSocket(void)
{
   int socketValue = socket(AF_INET, SOCK_DGRAM, 0);
   if (socketValue < 0) return -1;

   int receiveBufferBytes = 1024 * 1024;   // absorb a keyframe burst even if the reader hiccups
   int bufferRc = setsockopt(socketValue, SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes, sizeof receiveBufferBytes);
   static int bufferWarned;   // sockets reopen every reconnect attempt; one line per run is plenty
   if (bufferRc != 0 && !bufferWarned) { logWarn("[cst] SO_RCVBUF 1MB failed rc=%d\n", bufferRc); bufferWarned = 1; }

   struct sockaddr_in localAddress;
   memset(&localAddress, 0, sizeof localAddress);
   localAddress.sin_family = AF_INET;
   localAddress.sin_port = htons(CLIENT_PORT);
   localAddress.sin_addr.s_addr = htonl(INADDR_ANY);
   if (bind(socketValue, (struct sockaddr *)&localAddress, sizeof localAddress) < 0) {
      socketclose(socketValue);
      return -1;
   }
   return socketValue;
}

int discoverServer(int socketValue, struct sockaddr_in *serverAddress, int timeoutMs)
{
   setReceiveTimeout(socketValue, 500);
   uint64_t deadlineUs = getTimeUs() + (uint64_t)timeoutMs * 1000;
   while (getTimeUs() < deadlineUs) {
      char packet[PACKET_MAX];
      struct sockaddr_in fromAddress;
      socklen_t fromLength = sizeof fromAddress;
      int length = recvfrom(socketValue, packet, sizeof packet - 1, 0, (struct sockaddr *)&fromAddress, &fromLength);
      if (length <= 0) continue;
      packet[length] = 0;
      if (strncmp(packet, "CELLSTREAM", 10) == 0) { *serverAddress = fromAddress; return 1; }
   }
   return 0;
}

static const char *skipDigits(const char *cursor, long *value)
{
   if (*cursor < '0' || *cursor > '9') return NULL;
   long parsed = 0;
   while (*cursor >= '0' && *cursor <= '9') parsed = parsed * 10 + (*cursor++ - '0');
   *value = parsed;
   return cursor;
}

long parseNumberAfter(const char *text, const char *prefix)
{
   if (strncmp(text, prefix, strlen(prefix)) != 0) return -1;
   long value;
   return skipDigits(text + strlen(prefix), &value) ? value : -1;
}

// same, but for numbers too big for a long - `long` is only 32 bits here, and the server's clock is
// microseconds since 2020, which is far past what that holds. parsing it as a long came out negative and
// silently failed every clock sync.
long long parseBigNumberAfter(const char *text, const char *prefix)
{
   if (strncmp(text, prefix, strlen(prefix)) != 0) return -1;
   const char *cursor = text + strlen(prefix);
   if (*cursor < '0' || *cursor > '9') return -1;

   long long value = 0;
   while (*cursor >= '0' && *cursor <= '9') value = value * 10 + (*cursor++ - '0');
   return value;
}

