#pragma once

// tcp accept loop and command dispatch for simple-debug-bridge.
// listens on port 8785, dispatches one-line commands from the pc client.

#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "dbg.h"
#include "vsh.h"
#include "syscall.h"

#define SDB_PORT      8785
#define SDB_BUF_MAX   512
#define SDB_OK        "OK"
#define SDB_ERR       "ERR"

// sys_sm_shutdown modes (psdevwiki) — see syscall.h sysPower()
enum {
    POWER_SHUTDOWN   = 0x1100,
    POWER_REBOOT     = 0x1200,
    POWER_VSH_REBOOT = 0x0200
};

// teardown state — only relevant when something calls sys_prx_stop_module
// on us (hot reload during dev). on power-off / reboot lv2 tears the whole
// system down externally.
static volatile int      serverRunning  = 0;
static int               serverListenFd = -1;
static sys_ppu_thread_t  serverThreadId = 0;

// send a full buffer, retrying partial writes
static inline int serverSend(int fd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = send(fd, p, remaining, 0);
        if (n <= 0) return -1;
        p += n;
        remaining -= n;
    }
    return len;
}

// send a text line (no binary payload)
static inline int serverSendLine(int fd, const char *line)
{
    int len = 0;
    while (line[len]) len++;
    if (serverSend(fd, line, len) < 0) return -1;
    return serverSend(fd, "\n", 1);
}

// read one line from client (up to \n), null-terminates, returns length or -1
static int serverRecvLine(int fd, char *buf, int maxLen)
{
    int off = 0;
    while (off < maxLen - 1) {
        int n = recv(fd, buf + off, 1, 0);
        if (n <= 0) return -1;
        if (buf[off] == '\n') {
            buf[off] = '\0';
            // strip trailing \r
            if (off > 0 && buf[off - 1] == '\r') buf[--off] = '\0';
            return off;
        }
        off++;
    }
    buf[off] = '\0';
    return off;
}

// match a command prefix, return pointer to args (after space) or NULL
static inline const char *serverMatchCmd(const char *line, const char *cmd)
{
    int i = 0;
    while (cmd[i]) {
        if (line[i] != cmd[i]) return 0;
        i++;
    }
    if (line[i] == '\0') return line + i; // command with no args
    if (line[i] == ' ')  return line + i + 1; // skip space before args
    return 0;
}

// dispatch a single command from the client. power commands fire the
// syscall directly — lv2 tears the whole system down, so there's no need
// to stop the accept loop or close sockets first (same situation as a
// power-button shutdown, which ftp/sdm survive fine).
static void serverHandleClient(int cli)
{
    char buf[SDB_BUF_MAX];

    int len = serverRecvLine(cli, buf, sizeof buf);
    if (len <= 0) return;

    // skip logging ping — client polls it every few seconds and would
    // otherwise fill dbg.txt with noise.
    if (!serverMatchCmd(buf, "ping")) {
        dbgLog("[sdb] cmd: %s\n", buf);
    }

    if (serverMatchCmd(buf, "ping")) {
        serverSendLine(cli, SDB_OK);
    }
    else if (serverMatchCmd(buf, "restart-ps3")) {
        serverSendLine(cli, SDB_OK " rebooting");
        sysPower(POWER_REBOOT);
    }
    else if (serverMatchCmd(buf, "restart-xmb")) {
        serverSendLine(cli, SDB_OK " restarting xmb");
        sysPower(POWER_VSH_REBOOT);
    }
    else if (serverMatchCmd(buf, "shutdown")) {
        serverSendLine(cli, SDB_OK " shutting down");
        sysPower(POWER_SHUTDOWN);
    }
    else if (serverMatchCmd(buf, "screenshot")) {
        serverSendLine(cli, SDB_ERR " not implemented yet");
    }
    else {
        serverSendLine(cli, SDB_ERR " unknown command");
    }
}

// open a listening tcp socket on the given port, or -1 on any failure.
// caller retries the whole thing — keeps socket+bind+listen atomic so we
// never end up with a half-initialised fd lingering across retries.
static int serverListen(uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a;
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { socketclose(s); return -1; }
    if (listen(s, 2) < 0)                             { socketclose(s); return -1; }
    return s;
}

static void serverThread(uint64_t arg)
{
    (void)arg;
    dbgLog("[sdb] server thread start\n");

    // wait for xmb readiness before binding
    int ticks = 0;
    while (!isXmbReady()) {
        if (!serverRunning) {
            dbgLog("[sdb] cancelled during xmb wait\n");
            sys_ppu_thread_exit(0);
            return;
        }
        sys_timer_sleep(1);
        if (++ticks > 60) {
            dbgLog("[sdb] xmb ready timeout\n");
            sys_ppu_thread_exit(0);
            return;
        }
    }
    dbgLog("[sdb] xmb ready\n");

    int fd      = -1;
    int retries = 0;
    while (fd < 0 && retries < 30) {
        if (!serverRunning) {
            sys_ppu_thread_exit(0);
            return;
        }
        fd = serverListen(SDB_PORT);
        if (fd < 0) {
            dbgLog("[sdb] listen failed, retrying\n");
            sys_timer_sleep(2);
            retries++;
        }
    }
    if (fd < 0) {
        dbgLog("[sdb] giving up — port %d unavailable\n", SDB_PORT);
        sys_ppu_thread_exit(0);
        return;
    }

    // 1-second timeout so accept() wakes periodically to check serverRunning
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    serverListenFd = fd;
    dbgLog("[sdb] listening on :%d\n", SDB_PORT);

    while (serverRunning) {
        struct sockaddr_in ra;
        socklen_t al = sizeof ra;
        int c = accept(fd, (struct sockaddr *)&ra, &al);
        if (c < 0) continue;
        if (!serverRunning) {
            shutdown(c, SHUT_RDWR);
            socketclose(c);
            break;
        }
        serverHandleClient(c);
        shutdown(c, SHUT_RDWR);
        socketclose(c);
    }

    socketclose(fd);
    serverListenFd = -1;
    dbgLog("[sdb] server thread exit\n");
    sys_ppu_thread_exit(0);
}

// spawn the accept-loop thread.
static void serverStart(void)
{
    serverRunning = 1;
    sys_ppu_thread_t tid = 0;
    sys_ppu_thread_create(&tid, serverThread, 0, 0x400, 0x4000,
                          SYS_PPU_THREAD_CREATE_JOINABLE, "sdb");
    serverThreadId = tid;
}

// stop the server and wait for the thread to exit. only called from _stop
// (sys_prx_stop_module path) — never from the server thread itself, so no
// self-join guard needed.
static void serverStop(void)
{
    dbgLog("[sdb] serverStop\n");
    serverRunning = 0;

    // shutdown listen socket to wake accept() immediately
    if (serverListenFd >= 0) {
        shutdown(serverListenFd, SHUT_RDWR);
    }

    if (serverThreadId != 0) {
        uint64_t exitCode;
        sys_ppu_thread_join(serverThreadId, &exitCode);
        serverThreadId = 0;
    }

    dbgLog("[sdb] serverStop done\n");
}
