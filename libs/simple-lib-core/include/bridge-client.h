#pragma once

/* simple-debug-bridge producer client.
 *
 * Plugins and apps call registerWithBridge() once at startup. The log sink
 * is installed synchronously so any log emitted from that moment on is
 * captured. A background thread:
 *   1. opens a loopback TCP socket to the bridge on port 8785, retrying
 *      until the bridge listener is up (cold-boot race);
 *   2. sends the handshake "REGISTER <kind> <name>\n"; and
 *   3. drains the pre-connect line backlog, then flips to live forwarding.
 *
 * Plugin work (FTP listener, disc-mount HTTP listener, ...) does NOT wait
 * for registration -- the registration thread runs in parallel.
 *
 * Connect retries are HARD-CAPPED at BRIDGE_CONNECT_TRIES (30 s total at
 * 200 ms cadence); the thread exits silently if the bridge never appears,
 * so there is no possibility of spamming the loopback indefinitely. */

#include <stdint.h>
#include <sys/timer.h>
#include <sys/ppu_thread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "dbg.h"
#include "printf.h"
#include "thread.h"
#include "wire.h"
#include "log-backlog.h"

#define BRIDGE_LOOPBACK_PORT   8785
#define BRIDGE_CONNECT_TRIES   150            // 150 * 200ms = 30s ceiling
#define BRIDGE_RETRY_USEC      (200 * 1000)

// pre-connect line backlog. logInfo/Warn/Error fired during plugin startup
// (xmb-ready wait, mounts, listener-retry races) happens before the TCP
// connect to the bridge completes; without this ring those lines never
// reach the host's Logs tab. drained on successful REGISTER.
static LogBacklog bridgeBacklog;

typedef struct {
   const char *kind;
   const char *name;
} BridgeRegistration;

static int bridgeSocket = -1;

// log sink installed by registerWithBridge -- fires AFTER the dbg.txt
// write so disk remains the source of truth. while the bridge socket isn't
// up yet, queue into the local ring; once it is, push live.
static inline void pushLogToBridge(const char *line, int len)
{
   if (bridgeSocket < 0) {
      pushLogBacklog(&bridgeBacklog, line, len);
      return;
   }
   if (sendFrame(bridgeSocket, "LOG", line, len) < 0) {
      socketclose(bridgeSocket);
      bridgeSocket = -1;
   }
}

// drain callback: send each buffered line through the now-live socket.
static int sendBacklogLine(const char *data, int len, void *user)
{
   int socketFd = *(int *)user;
   return sendFrame(socketFd, "LOG", data, len);
}

// background thread: retries the connect until the bridge accepts (up to
// BRIDGE_CONNECT_TRIES), sends the REGISTER handshake, and installs the
// log sink. exits silently on timeout -- caller's logs keep flowing to
// dbg.txt only, which is acceptable for early-startup errors per design.
static void runBridgeRegistration(uint64_t arg)
{
   BridgeRegistration *registration = (BridgeRegistration *)(uintptr_t)arg;

   struct sockaddr_in address;
   address.sin_family      = AF_INET;
   address.sin_port        = htons(BRIDGE_LOOPBACK_PORT);
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   int connectedSocket = -1;
   for (int attempt = 0; attempt < BRIDGE_CONNECT_TRIES; attempt++) {
      connectedSocket = socket(AF_INET, SOCK_STREAM, 0);
      if (connectedSocket >= 0 &&
          connect(connectedSocket, (struct sockaddr *)&address, sizeof address) == 0) break;
      if (connectedSocket >= 0) { socketclose(connectedSocket); connectedSocket = -1; }
      sys_timer_usleep(BRIDGE_RETRY_USEC);
   }
   if (connectedSocket < 0) { sys_ppu_thread_exit(0); return; }

   char handshake[128];
   int handshakeLen = snprintf(handshake, sizeof handshake, "REGISTER %s %s\n",
                               registration->kind, registration->name);
   if (sendBytes(connectedSocket, handshake, handshakeLen) < 0) {
      socketclose(connectedSocket);
      sys_ppu_thread_exit(0);
      return;
   }

   bridgeSocket = connectedSocket;
   drainLogBacklog(&bridgeBacklog, sendBacklogLine, &bridgeSocket);
   // first line emitted through the sink -- confirms registration end-to-end
   // and gives the host a guaranteed marker even for silent plugins.
   logInfo("[%s] bridge link up\n", registration->name);
   sys_ppu_thread_exit(0);
}

// fire-and-forget: spawn the registration thread and return immediately.
// caller-owned strings must outlive the thread (literals are fine). the
// sink is installed synchronously so any logs emitted before (or during)
// the TCP connect get queued for replay after REGISTER.
static inline void registerWithBridge(const char *kind, const char *name)
{
   if (bridgeSocket >= 0) return;

   setLogSink(pushLogToBridge);

   static BridgeRegistration registration;   // single-shot per plugin
   registration.kind = kind;
   registration.name = name;

   sys_ppu_thread_t threadId = 0;
   spawnThread(&threadId, runBridgeRegistration, (uint64_t)(uintptr_t)&registration,
               THREAD_STACK_SIZE_8KB, "sdb_reg");
}