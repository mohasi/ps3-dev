#pragma once

// tcp accept loop, host vs producer demux, log forwarding, and command
// dispatch for simple-debug-bridge.
//
// listens on port 8785. up to SDB_MAX_HOSTS persistent host clients (the
// desktop client, the mcp server, ad-hoc scripts) + up to SDB_MAX_PLUGINS
// producer plugins + one app producer (newest wins). commands from all
// hosts run one at a time, and every log line goes to every host.
// each request is "<cmd>[ args]\n" (with optional raw upload bytes
// after the newline for upload commands like push-file). each reply
// is the framed format "<STATUS> <n>\n[<n bytes>]" - see sendFrame()
// / sendFrameHeader() in wire.h.
//
// command handlers live in the cmd-*.h headers; this file only owns
// the listener, the connection demux, and the dispatch table.

#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/synchronization.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "dbg.h"
#include "thread.h"
#include "vsh.h"
#include "log-backlog.h"
#include "plugin.h"
#include "pkg.h"

#include "cmd-common.h"
#include "cmd-introspect.h"
#include "cmd-file.h"
#include "cmd-trace.h"
#include "cmd-read-mem.h"
#include "cmd-stat-tree.h"
#include "cmd-pad.h"
#include "cmd-game.h"

#define SDB_PORT          8785
#define SDB_BUF_MAX       512
#define SDB_LOG_BODY_MAX  4096
#define SDB_MAX_HOSTS     4

// sys_sm_shutdown modes (psdevwiki) - see syscall.h sysPower()
enum {
   POWER_SHUTDOWN   = 0x1100,
   POWER_REBOOT     = 0x1200,
   POWER_VSH_REBOOT = 0x0200
};

// teardown state - only relevant when something calls sys_prx_stop_module
// on us (hot reload during dev). on power-off / reboot lv2 tears the whole
// system down externally.
static volatile int      isServerRunning = 0;
static int               serverListenFd  = -1;
static int               hostFds[SDB_MAX_HOSTS]; // connected hosts, -1 when free
static sys_ppu_thread_t  serverThreadId  = 0;

// serializes command dispatch and every write to a host socket. replies
// happen on that host's own thread; producer LOG frames are forwarded from
// producer threads and fan out to all hosts. without this lock a log write
// can interleave bytes inside a reply payload and desync a host's framed
// reader, and two hosts could run commands at the same time.
static sys_lwmutex_t     serverHostLock;

// active producer registry. one app slot (newest wins - if a second app
// registers we drop the previous one) and N plugin slots. logs flow from any
// registered producer to the host as soon as the host is connected.
#define SDB_MAX_PLUGINS 8
#define SDB_NAME_MAX    32

typedef struct {
   int  fd;
   char name[SDB_NAME_MAX];
} ProducerSlot;

static ProducerSlot appSlot    = { -1, { 0 } };
static ProducerSlot pluginSlots[SDB_MAX_PLUGINS];
static sys_lwmutex_t serverRegistryLock;

// pre-connect ring buffer. producers (and the bridge itself) start logging
// well before the host attaches; without a backlog those lines would be
// silently dropped and the operator would never see plugin startup output
// in the Logs tab. drained on host connect; older lines overwritten if the
// host stays away too long.
static LogBacklog logBacklog;

// drain callback: forward each buffered line as a LOG frame to the host.
static int sendBacklogToHost(const char *data, int len, void *user)
{
   int fd = *(int *)user;
   if (sendFrame(fd, "LOG", data, len) < 0) {
      shutdown(fd, SHUT_RDWR);
      return -1;
   }
   return 0;
}

// forward a LOG line from a producer thread to every connected host. takes
// the host lock so the "LOG <n>\n<bytes>" frame never interleaves with a
// reply to a host command. when no host is connected, queues into the
// ring buffer so the lines are replayed on the next host-connect.
static void forwardLogToHost(const char *buf, int len)
{
   lock(&serverHostLock);
   int sent = 0;
   for (int i = 0; i < SDB_MAX_HOSTS; i++) {
      int fd = hostFds[i];
      if (fd < 0) continue;
      sent = 1;
      if (sendFrame(fd, "LOG", buf, len) < 0) {
         shutdown(fd, SHUT_RDWR);   // that host's own thread notices and cleans up
      }
   }
   if (!sent) pushLogBacklog(&logBacklog, buf, len);
   unlock(&serverHostLock);
}

// claim a host slot for a new connection. caller holds serverHostLock.
static int addHost(int fd)
{
   for (int i = 0; i < SDB_MAX_HOSTS; i++) {
      if (hostFds[i] < 0) { hostFds[i] = fd; return i; }
   }
   return -1;
}

static int countHosts(void)
{
   int hosts = 0;
   for (int i = 0; i < SDB_MAX_HOSTS; i++) {
      if (hostFds[i] >= 0) hosts++;
   }
   return hosts;
}

// dispatch one command from the client. returns 0 on success, -1 on send
// failure (caller should drop the connection). power commands fire the
// syscall directly - lv2 tears the whole system down.
static int dispatchCommand(int cli, char *buf)
{
   // skip logging ping - client polls it every few seconds and would
   // otherwise fill dbg.txt with noise.
   if (!matchCommand(buf, "ping")) {
      logInfo("[sdb] cmd: %s\n", buf);
   }

   if (matchCommand(buf, "ping")) {
      return sendReply(cli, SDB_OK, "");
   }
   // vsh flags an improper shutdown on next boot unless /dev_hdd0/tmp/turnoff
   // is removed before sys_sm_shutdown. xai_plugin / evilnat cfw power options
   // do the same thing - see apps/xai_plugin/xai_plugin/functions.cpp rebootXMB().
   if (matchCommand(buf, "restart-ps3")) {
      sendReply(cli, SDB_OK, "rebooting");
      removeFilePath("/dev_hdd0/tmp/turnoff");
      sysPower(POWER_REBOOT);
      return 0;
   }
   if (matchCommand(buf, "restart-xmb")) {
      sendReply(cli, SDB_OK, "restarting xmb");
      removeFilePath("/dev_hdd0/tmp/turnoff");
      sysPower(POWER_VSH_REBOOT);
      return 0;
   }
   if (matchCommand(buf, "shutdown")) {
      sendReply(cli, SDB_OK, "shutting down");
      removeFilePath("/dev_hdd0/tmp/turnoff");
      sysPower(POWER_SHUTDOWN);
      return 0;
   }
   if (matchCommand(buf, "module-list")) {
      cmdModuleList(cli);
      return 0;
   }
   if (matchCommand(buf, "process-list")) {
      cmdProcessList(cli);
      return 0;
   }

   const char *args;
   char  name[PLUGIN_NAME_MAX];
   uint32_t size;
   char  reply[128];

   if      ((args = matchCommand(buf, "module-info"))      != 0) cmdModuleInfo     (cli, args);
   else if ((args = matchCommand(buf, "module-trace-on"))  != 0) cmdModuleTraceOn  (cli, args);
   else if ((args = matchCommand(buf, "module-trace-off")) != 0) cmdModuleTraceOff (cli, args);
   else if ((args = matchCommand(buf, "read-mem"))         != 0) cmdReadMem        (cli, args);
   else if ((args = matchCommand(buf, "process-info"))     != 0) cmdProcessInfo    (cli, args);
   else if ((args = matchCommand(buf, "pull-file"))        != 0) cmdPullFile       (cli, args);
   else if ((args = matchCommand(buf, "push-file"))        != 0) cmdPushFile       (cli, args);
   else if ((args = matchCommand(buf, "delete-file"))      != 0) cmdDeleteFile     (cli, args);
   else if ((args = matchCommand(buf, "list-dir"))         != 0) cmdListDir        (cli, args);
   else if ((args = matchCommand(buf, "stat-tree"))        != 0) cmdStatTree       (cli, args);
   else if ((args = matchCommand(buf, "pad"))              != 0) cmdPad            (cli, args);
   else if ((args = matchCommand(buf, "launch"))           != 0) cmdLaunchTitle    (cli, args);
   else if ((args = matchCommand(buf, "exit-game"))        != 0) cmdExitGame       (cli, args);
   else if ((args = matchCommand(buf, "vsh-plugin-install")) != 0) {
      if (!parseNameAndSize(args, name, sizeof name, &size)) {
         sendReply(cli, SDB_ERR, "usage: vsh-plugin-install <name> <size>");
      } else if (installPlugin(cli, name, size) < 0) {
         sendReply(cli, SDB_ERR, "install failed");
      } else {
         snprintf(reply, sizeof reply, "installed %s (%u bytes)", name, (unsigned)size);
         sendReply(cli, SDB_OK, reply);
      }
   }
   else if ((args = matchCommand(buf, "vsh-plugin-uninstall")) != 0) {
      if (args[0] == '\0') {
         sendReply(cli, SDB_ERR, "usage: vsh-plugin-uninstall <name>");
      } else if (uninstallPlugin(args) < 0) {
         sendReply(cli, SDB_ERR, "uninstall failed");
      } else {
         snprintf(reply, sizeof reply, "uninstalled %s", args);
         sendReply(cli, SDB_OK, reply);
      }
   }
   else if ((args = matchCommand(buf, "pkg-uninstall")) != 0) {
      if (!isValidTitleId(args)) {
         sendReply(cli, SDB_ERR, "usage: pkg-uninstall <TITLE_ID>");
      } else {
         uint64_t freed = 0;
         int rc = uninstallPkg(args, &freed);
         if (rc < 0) {
            sendReply(cli, SDB_ERR, "uninstall failed");
         } else if (rc > 0) {
            sendReply(cli, SDB_ERR, "not installed");
         } else {
            snprintf(reply, sizeof reply, "uninstalled %s (%llu bytes)",
                     args, (unsigned long long)freed);
            sendReply(cli, SDB_OK, reply);
         }
      }
   }
   else if ((args = matchCommand(buf, "pkg-install")) != 0) {
      // wire: "pkg-install <name> <clean> <size>". host shapes the URL
      // params; HttpBridge appends `<size>` from the POST body length.
      char  pkgName[SDB_NAME_MAX];
      uint32_t clean = 0;
      // reuse parseNameAndSize for the leading "<name> <clean>" pair,
      // then take the trailing size by hand.
      if (!parseNameAndSize(args, pkgName, sizeof pkgName, &clean)) {
         sendReply(cli, SDB_ERR, "usage: pkg-install <name> <clean> <size>");
      } else {
         const char *tail = args;
         while (*tail && *tail != ' ') tail++;          // skip name
         while (*tail == ' ') tail++;
         while (*tail && *tail != ' ') tail++;          // skip clean
         while (*tail == ' ') tail++;
         uint32_t size = 0;
         int digits = 0;
         while (*tail >= '0' && *tail <= '9') { size = size * 10 + (uint32_t)(*tail - '0'); tail++; digits++; }
         if (digits == 0) {
            sendReply(cli, SDB_ERR, "usage: pkg-install <name> <clean> <size>");
         } else if (stagePkgUpload(cli, pkgName, size) < 0) {
            sendReply(cli, SDB_ERR, "stage failed");
         } else {
            // staged ok - extract into /dev_hdd0/game/<TITLE_ID>/. read
            // title-id from the pkg's PARAM.SFO so the host never has to
            // know it. `clean` controls whether an existing install is
            // wiped first (matches xmb "reinstall" behavior).
            char pkgPath[FILE_PATH_MAX];
            buildStagePath(pkgPath, sizeof pkgPath, pkgName);
            char     titleId[PKG_TITLE_LEN + 1] = {0};
            uint32_t files = 0;
            uint64_t bytes = 0;
            if (installPkg(pkgPath, (int)clean, titleId, &files, &bytes) < 0) {
               sendReply(cli, SDB_ERR, "extract failed");
            } else {
               snprintf(reply, sizeof reply, "installed %s (%u files, %llu bytes)",
                        titleId, (unsigned)files, (unsigned long long)bytes);
               sendReply(cli, SDB_OK, reply);
            }
         }
      }
   }
   else {
      sendReply(cli, SDB_ERR, "unknown command");
   }
   return 0;
}

// persistent host session: read framed commands (newline-terminated) and
// send framed replies until the host disconnects or a send fails. each
// dispatch grabs the host write mutex so a producer LOG forward cannot
// splice bytes inside the reply frame.
static void handleHostSession(int cli)
{
   char buf[SDB_BUF_MAX];
   while (isServerRunning) {
      int len = receiveLine(cli, buf, sizeof buf);
      if (len <= 0) return;
      lock(&serverHostLock);
      int rc = dispatchCommand(cli, buf);
      unlock(&serverHostLock);
      if (rc < 0) return;
   }
}

static inline void copyProducerName(ProducerSlot *slot, const char *name)
{
   int i = 0;
   while (i < SDB_NAME_MAX - 1 && name[i]) { slot->name[i] = name[i]; i++; }
   slot->name[i] = '\0';
}

// register a producer in the app slot or a free plugin slot. on app
// registration any previous app socket is shut down (newest wins). returns
// the slot pointer, or NULL if no plugin slot was free.
static ProducerSlot *registerProducer(int cli, int isApp, const char *name)
{
   ProducerSlot *slot = 0;
   lock(&serverRegistryLock);

   if (isApp) {
      if (appSlot.fd >= 0) {
         logWarn("[sdb] replacing app producer %s with %s\n", appSlot.name, name);
         shutdown(appSlot.fd, SHUT_RDWR);
      }
      appSlot.fd = cli;
      copyProducerName(&appSlot, name);
      slot = &appSlot;
   } else {
      for (int i = 0; i < SDB_MAX_PLUGINS; i++) {
         if (pluginSlots[i].fd < 0) {
            pluginSlots[i].fd = cli;
            copyProducerName(&pluginSlots[i], name);
            slot = &pluginSlots[i];
            break;
         }
      }
   }

   unlock(&serverRegistryLock);
   return slot;
}

static void unregisterProducer(ProducerSlot *slot)
{
   lock(&serverRegistryLock);
   slot->fd = -1;
   slot->name[0] = '\0';
   unlock(&serverRegistryLock);
}

// producer session: forward incoming "LOG <n>\n<bytes>" frames to the host
// (if connected). also accepts "BYE\n" as a graceful disconnect signal.
static void handleProducerSession(int cli, ProducerSlot *slot)
{
   char buf[SDB_BUF_MAX];
   while (isServerRunning) {
      int len = receiveLine(cli, buf, sizeof buf);
      if (len <= 0) return;

      const char *args;
      if ((args = matchCommand(buf, "LOG")) != 0) {
         uint64_t n = 0;
         if (parseUInt64(args, &n) == 0 || n == 0 || n > SDB_LOG_BODY_MAX) {
            logWarn("[sdb] producer %s: bad LOG frame\n", slot->name);
            return;
         }
         char body[SDB_LOG_BODY_MAX];
         if (receiveExact(cli, body, (int)n) < 0) return;
         forwardLogToHost(body, (int)n);
      }
      else if (matchCommand(buf, "BYE")) {
         return;
      }
      else {
         logWarn("[sdb] producer %s: unknown frame: %s\n", slot->name, buf);
      }
   }
}

// per-connection thread arg: just the fd boxed as a uint64_t.
static void runConnHandler(uint64_t arg)
{
   int cli = (int)arg;

   // first line decides the role of this socket. expected forms:
   //   "REGISTER plugin <name>\n"  - producer plugin
   //   "REGISTER app <name>\n"     - producer app
   //   anything else               - legacy host control session
   char first[SDB_BUF_MAX];
   int len = receiveLine(cli, first, sizeof first);
   if (len <= 0) goto done;

   const char *args = matchCommand(first, "REGISTER");
   if (args) {
      int isApp = 0;
      const char *name = 0;
      if ((name = matchCommand(args, "app")) != 0)    isApp = 1;
      else if ((name = matchCommand(args, "plugin")) != 0) isApp = 0;

      if (!name || !*name) {
         logWarn("[sdb] bad REGISTER frame: %s\n", first);
         goto done;
      }
      ProducerSlot *slot = registerProducer(cli, isApp, name);
      if (!slot) {
         logWarn("[sdb] no producer slot for %s\n", name);
         goto done;
      }
      // producers only push LOG frames outbound and may go minutes
      // between writes; the host-oriented 30s recv timeout from the
      // accept loop would silently drop the slot. clear it so the
      // producer stays registered until it actively disconnects.
      struct timeval none = { 0, 0 };
      setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
      logInfo("[sdb] producer registered: %s %s\n", isApp ? "app" : "plugin", name);
      handleProducerSession(cli, slot);
      unregisterProducer(slot);
      logInfo("[sdb] producer disconnected: %s\n", name);
   }
   else {
      // publish the slot under the host lock so the log forwarder never
      // races between "fd is valid" and "socket still open" — the same lock
      // guards every write to every host fd.
      lock(&serverHostLock);
      int slotIndex = addHost(cli);
      if (slotIndex < 0) {
         unlock(&serverHostLock);
         logWarn("[sdb] rejecting host - all %d slots in use\n", SDB_MAX_HOSTS);
         sendReply(cli, SDB_ERR, "busy");
      } else {
         // drain any logs buffered while no host was connected (plugin
         // startup, bridge startup, producer registrations) so the
         // Debug Logs tab shows the full history, not just post-connect.
         drainLogBacklog(&logBacklog, sendBacklogToHost, &cli);
         // the first line we already consumed is a real command; dispatch
         // it before entering the read loop.
         int rc = dispatchCommand(cli, first);
         int hosts = countHosts();
         unlock(&serverHostLock);
         logInfo("[sdb] host connected (%d of %d)\n", hosts, SDB_MAX_HOSTS);
         if (rc >= 0) handleHostSession(cli);
         lock(&serverHostLock);
         hostFds[slotIndex] = -1;
         hosts = countHosts();
         unlock(&serverHostLock);
         logInfo("[sdb] host disconnected (%d left)\n", hosts);
      }
   }

done:
   shutdown(cli, SHUT_RDWR);
   socketclose(cli);
   sys_ppu_thread_exit(0);
}

// open a listening tcp socket on the given port, or -1 on any failure.
// caller retries the whole thing - keeps socket+bind+listen atomic so we
// never end up with a half-initialised fd lingering across retries.
static int openListener(uint16_t port)
{
   int s = socket(AF_INET, SOCK_STREAM, 0);
   if (s < 0) return -1;

   struct sockaddr_in a;
   a.sin_family      = AF_INET;
   a.sin_port        = htons(port);
   a.sin_addr.s_addr = htonl(INADDR_ANY);

   if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { socketclose(s); return -1; }
   if (listen(s, 8) < 0)                             { socketclose(s); return -1; }
   return s;
}

static void runAcceptLoop(uint64_t arg)
{
   (void)arg;
   logInfo("[sdb] server thread start\n");

   // wait for xmb readiness before binding
   int ticks = 0;
   while (!isXmbReady()) {
      if (!isServerRunning) {
         logInfo("[sdb] cancelled during xmb wait\n");
         sys_ppu_thread_exit(0);
         return;
      }
      sys_timer_sleep(1);
      if (++ticks > 60) {
         logError("[sdb] xmb ready timeout\n");
         sys_ppu_thread_exit(0);
         return;
      }
   }
   logInfo("[sdb] xmb ready\n");

   // The VFS is intentionally NOT brought up here: the bridge's file commands only
   // need cellFs (HDD / dev_flash / kernel FAT32 USB), which the dispatch wrappers
   // route to without initVfs. Skipping bringup keeps the exFAT/NTFS drivers out of
   // this sprx (this linker can't strip them once initVfs/shutdownVfs are linked).
   // Use FTP or the file-manager to reach exFAT/NTFS sticks.

   int fd      = -1;
   int retries = 0;
   while (fd < 0 && retries < 30) {
      if (!isServerRunning) {
         sys_ppu_thread_exit(0);
         return;
      }
      fd = openListener(SDB_PORT);
      if (fd < 0) {
         logWarn("[sdb] listen failed, retrying\n");
         sys_timer_sleep(2);
         retries++;
      }
   }
   if (fd < 0) {
      logError("[sdb] giving up - port %d unavailable\n", SDB_PORT);
      sys_ppu_thread_exit(0);
      return;
   }

   // 1-second timeout so accept() wakes periodically to check isServerRunning
   struct timeval tv;
   tv.tv_sec = 1;
   tv.tv_usec = 0;
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

   serverListenFd = fd;
   logInfo("[sdb] listening on :%d\n", SDB_PORT);

   while (isServerRunning) {
      struct sockaddr_in ra;
      socklen_t al = sizeof ra;
      int c = accept(fd, (struct sockaddr *)&ra, &al);
      if (c < 0) continue;
      if (!isServerRunning) {
         shutdown(c, SHUT_RDWR);
         socketclose(c);
         break;
      }
      // per-client recv timeout so a wedged peer (e.g. powered off mid-
      // session) doesn't keep the slot indefinitely; the connection will
      // be dropped and the next reconnect attempt accepted.
      struct timeval ct;
      ct.tv_sec  = 30;
      ct.tv_usec = 0;
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &ct, sizeof ct);
      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof ct);

      // detached per-connection thread. handshake decides host vs producer
      // role; host slot claiming lives inside the thread so producers can
      // keep registering even when every host slot is taken.
      sys_ppu_thread_t tid = 0;
      spawnThread(&tid, runConnHandler, (uint64_t)c, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "bridge-conn");
      // detached - thread self-exits on disconnect and lv2 reclaims it.
   }

   socketclose(fd);
   serverListenFd = -1;
   logInfo("[sdb] server thread exit\n");
   sys_ppu_thread_exit(0);
}

// initialize state and mutexes, then spawn the accept-loop thread.
static void startServer(void)
{
   isServerRunning = 1;

   // serverHostLock is recursive: dispatchCommand runs under it, and any
   // logInfo inside dispatch re-enters via the bridge's own LOG tee. without
   // recursion that's an instant self-deadlock. registry lock is plain.
   createRecursiveLock(&serverHostLock);
   createLock(&serverRegistryLock);

   for (int i = 0; i < SDB_MAX_HOSTS; i++) hostFds[i] = -1;

   appSlot.fd = -1;
   appSlot.name[0] = '\0';
   for (int i = 0; i < SDB_MAX_PLUGINS; i++) {
      pluginSlots[i].fd = -1;
      pluginSlots[i].name[0] = '\0';
   }

   // accept loop thread is joinable so stopServer can wait for clean
   // teardown; per-connection threads (above) are detached.
   sys_ppu_thread_t tid = 0;
   spawnJoinableThread(&tid, runAcceptLoop, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "bridge-accept");
   serverThreadId = tid;
}

// stop the server and wait for the thread to exit. only called from _stop
// (sys_prx_stop_module path) - never from the server thread itself, so no
// self-join guard needed.
static void stopServer(void)
{
   logInfo("[sdb] stopServer\n");
   isServerRunning = 0;
   closeVirtualPad();   // don't leave a fake controller registered with lv2

   // wake any in-progress host recvs
   lock(&serverHostLock);
   for (int i = 0; i < SDB_MAX_HOSTS; i++) {
      if (hostFds[i] >= 0) shutdown(hostFds[i], SHUT_RDWR);
   }
   unlock(&serverHostLock);
   // wake any in-progress producer recvs
   lock(&serverRegistryLock);
   if (appSlot.fd >= 0) shutdown(appSlot.fd, SHUT_RDWR);
   for (int i = 0; i < SDB_MAX_PLUGINS; i++) {
      if (pluginSlots[i].fd >= 0) shutdown(pluginSlots[i].fd, SHUT_RDWR);
   }
   unlock(&serverRegistryLock);
   // shutdown listen socket to wake accept() immediately
   if (serverListenFd >= 0) {
      shutdown(serverListenFd, SHUT_RDWR);
   }

   if (serverThreadId != 0) {
      joinThread(serverThreadId);
      serverThreadId = 0;
   }
   logInfo("[sdb] stopServer done\n");
}
