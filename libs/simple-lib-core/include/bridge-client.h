#pragma once

// simple-debug-bridge producer client.
//
// Plugins and apps call registerWithBridge() once at startup. The log sink
// is installed synchronously so any log emitted from that moment on is
// captured. A background thread:
//   1. opens a loopback TCP socket to the bridge on port 8785, retrying
//      until the bridge listener is up (cold-boot race);
//   2. sends the handshake "REGISTER <kind> <name>\n"; and
//   3. drains the pre-connect line backlog, then flips to live forwarding.
//
// Plugin work (FTP listener, disc-mount HTTP listener, ...) does NOT wait
// for registration -- the registration thread runs in parallel.
//
// Connect retries are HARD-CAPPED at BRIDGE_CONNECT_TRIES (10 s total at
// 500 ms cadence); the thread exits silently if the bridge never appears,
// so there is no possibility of spamming the loopback indefinitely.

#include <stdint.h>
#include <sys/ppu_thread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "dbg.h"
#include "printf.h"
#include "thread.h"
#include "wire.h"
#include "log-backlog.h"
#include "string-utilities.h"

#define BRIDGE_LOOPBACK_PORT   8785
#define BRIDGE_CONNECT_TRIES   20             // 20 * 500ms = 10s ceiling
#define BRIDGE_RETRY_MS        500

// pre-connect line backlog. logInfo/Warn/Error fired during plugin startup
// (xmb-ready wait, mounts, listener-retry races) happens before the TCP
// connect to the bridge completes; without this ring those lines never
// reach the host's Logs tab. drained on successful REGISTER.
static LogBacklog bridgeBacklog;

// write lock: sendFrame is two send() calls (header + payload) and logs can
// fire concurrently (plugin main thread + HTTP listener, FTP listener + session
// threads, ...). without this lock two frames can interleave and corrupt the stream.
static sys_lwmutex_t bridgeWriteLock;
static int bridgeWriteLockInit = 0;

typedef struct {
   const char *kind;
   const char *name;
} BridgeRegistration;

static int bridgeSocket = -1;

static inline int isBridgeConnected(void) { return bridgeSocket >= 0; }

static inline void dropBridgeConnection(void)
{
   if (bridgeSocket >= 0) {
     socketclose(bridgeSocket);
     bridgeSocket = -1;
   }
}

// log sink installed by registerWithBridge -- fires AFTER the dbg.txt
// write so disk remains the source of truth. while the bridge socket isn't
// up yet, queue into the local ring; once it is, push live.
static inline void pushLogToBridge(const char *line, int len)
{
   if (!isBridgeConnected()) {
     pushLogBacklog(&bridgeBacklog, line, len);
     return;
   }

   lock(&bridgeWriteLock);
   int success = sendFrame(bridgeSocket, "LOG", line, len) == 0;
   unlock(&bridgeWriteLock);
   if (!success) dropBridgeConnection();
}

// drain callback: send each buffered line through the now-live socket.
static int sendBacklogLine(const char *data, int len, void *user)
{
   int socketFd = *(int *)user;
   return sendFrame(socketFd, "LOG", data, len);
}

// app-side request handler: bridge forwards host requests (capture today,
// future host->app rpcs) over the same socket. for now nothing is wired up,
// so any inbound line gets an immediate "not implemented" reply instead of
// being silently dropped.
static void runRequestHandler(uint64_t arg)
{
   (void)arg;
   static const char notImplemented[] = "request not implemented";

   while (isBridgeConnected()) {
     char line[128];
     int lineLength = receiveLine(bridgeSocket, line, sizeof line); // blocks
     if (lineLength <= 0) break;

     int success = sendFrame(bridgeSocket, "ERR", notImplemented, sizeof(notImplemented) - 1) == 0;
     if (!success) break;
   }

   dropBridgeConnection();
   exitThread();
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

   // connect to bridge
   int connectedSocket = -1;
   for (int attempt = 0; attempt < BRIDGE_CONNECT_TRIES; attempt++) {
     connectedSocket = socket(AF_INET, SOCK_STREAM, 0);
     int opened    = connectedSocket >= 0;
     int connected = opened && connect(connectedSocket, (struct sockaddr *)&address, sizeof address) == 0;
     if (connected) break;
     if (opened) { socketclose(connectedSocket); connectedSocket = -1; }
     sleepMs(BRIDGE_RETRY_MS);
   }
   if (connectedSocket < 0) goto done;

   // register with bridge
   char handshake[128];
   int handshakeLen = snprintf(handshake, sizeof handshake, "REGISTER %s %s\n", registration->kind, registration->name);
   int success = sendBytes(connectedSocket, handshake, handshakeLen) == handshakeLen;
   if (!success) {
     socketclose(connectedSocket);
     goto done;
   }

   // drain the backlog BEFORE flipping bridgeSocket to live, so pushLogToBridge
   // keeps queueing instead of racing with sendBacklogLine. once the drain finishes
   // bridgeSocket becomes visible and live logs start flowing directly.
   drainLogBacklog(&bridgeBacklog, sendBacklogLine, &connectedSocket);
   bridgeSocket = connectedSocket;

   // first line emitted through the sink -- confirms registration end-to-end
   // and gives the host a guaranteed marker even for silent plugins.
   logInfo("[%s] bridge link up\n", registration->name);

   // only apps service inbound requests today (capture, future host->app rpcs).
   // plugins never receive requests, so don't tie a thread up in a recv that
   // will never wake.
   int isApp = strEq(registration->kind, "app");
   if (isApp) {
     sys_ppu_thread_t handlerThreadId = 0;
     spawnThread(&handlerThreadId, runRequestHandler, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "bridge-client-req");
   }

done:
   exitThread();
}

// fire-and-forget: spawn the registration thread and return immediately.
// caller-owned strings must outlive the thread (literals are fine). the
// sink is installed synchronously so any logs emitted before (or during)
// the TCP connect get queued for replay after REGISTER.
static inline void registerWithBridge(const char *kind, const char *name)
{
   if (isBridgeConnected()) return;

   if (!bridgeWriteLockInit) {
     createLock(&bridgeWriteLock);
     bridgeWriteLockInit = 1;
   }

   setLogCallback(pushLogToBridge);

   static BridgeRegistration registration;   // single-shot per producer
   registration.kind = kind;
   registration.name = name;

   sys_ppu_thread_t threadId = 0;
   spawnThread(&threadId, runBridgeRegistration, (uint64_t)(uintptr_t)&registration,
               THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "bridge-client-reg");
}