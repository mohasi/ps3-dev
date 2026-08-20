// Simple CD Info - VSH plugin.
//
// Restores audio-CD metadata to the XMB. The stock lookup (AMG, dmr.allmusic.com)
// is dead, so we impersonate it. The firmware resolves that host through vsh's own
// gethostbyname; we hook it and, only for the AMG host, rewrite the name to loopback
// (127.0.0.1). The port it connects to is a constant inside the firmware's own lookup
// module, which patchAmgLookupPort rewrites, so the POST lands on our listener and
// port 80 stays free for webMAN.
// We then read the disc, look the album up on gnudb, build the AMG binary response
// on the fly (buildLiveResponse) and reply with it; the firmware's own parser turns
// it into album/artist/track text on screen. No XMB proxy setting is needed.

#include <sys/prx.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "dbg.h"
#include "vsh.h"
#include "thread.h"
#include "syscall.h"      // scCall1 (getpid)
#include "string-utilities.h"  // getStrLen, startsWith, findBytes, memSet
#include "wire.h"         // sendBytes - full-send loop over a socket
#include "export-hook.h"  // installExportHook - detour a vsh export at runtime
#include "proc-mem.h"     // writeProcMem - kernel poke (writes read-only pages)
#include "bridge-client.h"
#include "port-patch.h"   // patchAmgLookupPort - move the firmware's lookup off port 80
#include "toc.h"          // readCdToc - read the audio CD's track layout from the drive
#include "cddb.h"         // computeCddbDiscId + the gnudb lookup helpers
#include "lookup.h"       // buildLiveResponse - TOC -> gnudb -> AMG response

SYS_MODULE_INFO(SimpleCdInfo, 0, 1, 1);
SYS_MODULE_START(_start);

#define TAG "[cdi] "

#define LISTEN_PORT         8790   // any free port: patchAmgLookupPort sends the lookup here, leaving :80 to webMAN
#define PORT_PATCH_TRIES    30     // x3_amgsdk is loaded at boot, so this only covers a slow one
#define PORT_PATCH_RETRY_MS 2000
#define RESPONSE_MAX        16384  // ample: the 40 KB scratch arena caps the album at ~35 tracks (~6.5 KB body) first
#define HEN_HOOK_SETTLE_MS  5000   // extra grace period before patching vsh code on HEN, see serveThread

#define SYS_PROCESS_GETPID   1
#define NID_GETHOSTBYNAME    0x71f4c717u   // vsh sys_net export we detour
#define AMG_HOST_MARK        "allmusic"    // substring identifying the dead AMG lookup host
#define REDIRECT_IP          "127.0.0.1"   // loopback: the console talks to its own listener, network-independent

static char           responseBuffer[RESPONSE_MAX];

// open a bound+listening TCP socket, or -1. keeps socket+bind+listen atomic. binds
// loopback only: the firmware reaches us via 127.0.0.1 (redirected or proxied), so
// there is no reason to accept from the LAN.
static int openListener(uint16_t port)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) return -1;

   struct sockaddr_in address;
   memSet(&address, 0, sizeof address);
   address.sin_family      = AF_INET;
   address.sin_port        = htons(port);
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0) { socketclose(fd); return -1; }
   if (listen(fd, 4) < 0)                                         { socketclose(fd); return -1; }
   return fd;
}

// resident gethostbyname detour (installed once at startup). The XMB resolves the
// dead AMG host dmr.allmusic.com through vsh's own gethostbyname; when we see that
// name we poke it to loopback so the firmware's numeric fast-path returns 127.0.0.1
// and the disc-info POST lands on our listener. Every other lookup passes through
// untouched. Runs on whatever thread makes the DNS call, so it stays tiny: a bounded
// name check plus, at most, one kernel poke.
static void gethostbynameHook(const char *name)
{
   if (!name) return;
   int isAmgHost = 0;
   for (int i = 0; i < 48 && name[i]; i++)
      if (name[i] == 'a' && startsWith(name + i, AMG_HOST_MARK)) { isAmgHost = 1; break; }
   if (!isAmgHost) return;   // not the AMG host - leave the lookup alone

   // name is read-only rodata inside x3_amgsdk; the kernel poke (same one the code
   // hooks use) bypasses the page protection. Only poke when the original host is at
   // least as long as our replacement, so we never write past its buffer.
   static const char redirectIp[] = REDIRECT_IP;   // "127.0.0.1\0"
   if (getStrLen(name) + 1 < (int)sizeof redirectIp) return;
   uint32_t pid = (uint32_t)scCall1(SYS_PROCESS_GETPID, 0);
   int rc = writeProcMem(pid, (uint32_t)(uintptr_t)name, redirectIp, sizeof redirectIp);
   logInfo(TAG "dns: AMG host -> %s rc=0x%x\n", redirectIp, (unsigned)rc);
}

static void serveThread(uint64_t arg)
{
   (void)arg;

   // wait for the XMB before binding
   int ticks = 0;
   while (!isXmbReady()) {
      sleepMs(1000);
      if (++ticks > 60) { logError(TAG "xmb ready timeout\n"); exitThread(); return; }
   }

   // on HEN, isXmbReady() is already true the moment we get here -- the XMB was up
   // and running long before the user enabled HEN, unlike CFW's quiet cold boot. HEN
   // enable itself is still doing housekeeping (category refresh, unloading its own
   // enabler plugin) right about now, so patching live VSH code this early has been
   // reported to freeze the console on some HEN setups. Give that housekeeping room
   // to finish before we touch VSH's code pages.
   if (isHenActive()) {
      logInfo(TAG "hen detected, waiting %dms before patching vsh code\n", HEN_HOOK_SETTLE_MS);
      sleepMs(HEN_HOOK_SETTLE_MS);
   }

   // redirect the dead AMG host to us, so no XMB proxy setting is needed. Fail-safe:
   // if the export can't be resolved this logs and does nothing (networking untouched).
   int hookRc = installExportHook("sys_net", NID_GETHOSTBYNAME, (void (*)(void))gethostbynameHook);
   logInfo(TAG "dns hook install rc=%d\n", hookRc);

   // send the lookup to our port instead of :80. x3_amgsdk is part of the boot set, but
   // retry in case a slow boot has not got to it yet.
   int portRc = AMG_MODULE_NOT_LOADED;
   for (int retry = 0; portRc == AMG_MODULE_NOT_LOADED && retry < PORT_PATCH_TRIES; retry++) {
      portRc = patchAmgLookupPort(LISTEN_PORT);
      if (portRc == AMG_MODULE_NOT_LOADED) sleepMs(PORT_PATCH_RETRY_MS);
   }
   if (portRc < 0) logError(TAG "lookup port still 80, webman conflict remains (rc=%d)\n", portRc);

   int listenSocket = -1;
   for (int retry = 0; listenSocket < 0 && retry < 30; retry++) {
      listenSocket = openListener(LISTEN_PORT);
      if (listenSocket < 0) { logWarn(TAG "listen failed, retrying\n"); sleepMs(2000); }
   }
   if (listenSocket < 0) { logError(TAG "port %d unavailable, giving up\n", LISTEN_PORT); exitThread(); return; }
   logInfo(TAG "listening on :%d - AMG host redirected to " REDIRECT_IP ", no proxy needed\n", LISTEN_PORT);

   static const char emptyOk[] = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";

   for (;;) {
      struct sockaddr_in remote; socklen_t remoteLength = sizeof remote;
      int client = accept(listenSocket, (struct sockaddr *)&remote, &remoteLength);
      if (client < 0) { sleepMs(100); continue; }

      // read the request head so we can tell the CD lookup apart from other XMB web
      // traffic; the CD lookup is the only POST to /sdkrequest. The firmware sends the
      // POST then waits for our reply without closing, so the read ends on the recv
      // timeout, not on EOF - keep it short (the request is one small loopback segment).
      struct timeval readTimeout; readTimeout.tv_sec = 0; readTimeout.tv_usec = 500 * 1000;
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &readTimeout, sizeof readTimeout);
      char requestBuffer[2048]; int requestLength = 0, got;
      while (requestLength < (int)sizeof requestBuffer &&
             (got = recv(client, requestBuffer + requestLength, (int)sizeof requestBuffer - requestLength, 0)) > 0)
         requestLength += got;

      // only the CD lookup gets our crafted album; everything else gets an empty
      // 200 so answering the album can never poison background XMB requests.
      int isCdLookup = findBytes(requestBuffer, requestLength, "sdkrequest", 10) >= 0;

      // CD lookup: build the album live from the disc + gnudb. A failed build, or any non-lookup
      // traffic, gets an empty 200 so answering the album can never poison background XMB requests.
      int responseLength = 0;
      if (isCdLookup) {
         responseLength = buildLiveResponse(responseBuffer, sizeof responseBuffer);
         if (responseLength > 0)
            logInfo(TAG "sdkrequest %d bytes -> live gnudb reply %d bytes\n", requestLength, responseLength);
      }
      if (responseLength > 0) {
         sendBytes(client, responseBuffer, responseLength);
      } else {
         logInfo(TAG "%s %d bytes -> empty 200\n", isCdLookup ? "sdkrequest(no data)" : "other", requestLength);
         sendBytes(client, emptyOk, (int)sizeof emptyOk - 1);
      }
      shutdown(client, SHUT_RDWR);
      socketclose(client);
   }
}

int _start(uint64_t arg)
{
   (void)arg;
   registerWithBridge("plugin", "cdi");
   logInfo(TAG "_start\n");
   logBuildVersion();

   sys_ppu_thread_t tid;
   int rc = spawnThread(&tid, serveThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "cdi-serve");
   if (rc != 0) logError(TAG "serve thread spawn rc=0x%x\n", rc);
   return SYS_PRX_RESIDENT;
}

// No _stop, matching the other resident vsh plugins (disc-mount, ftp): the plugin
// stays loaded, the serve thread parks in accept(), and a full power-off drops it
// all with the rest of vsh. Nothing here to tear down on the shutdown path.
