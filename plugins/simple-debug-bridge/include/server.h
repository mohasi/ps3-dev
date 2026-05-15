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
#include "printf.h"
#include "string-utilities.h"
#include "fileio.h"
#include "installer.h"

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

// list every prx loaded into vsh.self.
// emits one "<id>\t<name>\t<filename>" line per module then "OK <count>".
static void cmdVshPluginList(int cli)
{
    static uint32_t ids[128];
    char     name[PRX_NAME_MAX];
    char     file[PRX_FILENAME_MAX];
    char     line[PRX_NAME_MAX + PRX_FILENAME_MAX + 32];
    uint32_t count;

    int32_t rc = prxList(ids, sizeof ids / sizeof ids[0], &count);
    if (rc < 0) {
        snprintf(line, sizeof line, SDB_ERR " prxList rc=0x%x", (unsigned)rc);
        serverSendLine(cli, line);
        return;
    }

    int sent = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (prxName((int32_t)ids[i], name, file) < 0) continue;
        snprintf(line, sizeof line, "%u\t%s\t%s", (unsigned)ids[i], name, file);
        serverSendLine(cli, line);
        sent++;
    }

    snprintf(line, sizeof line, SDB_OK " %d", sent);
    serverSendLine(cli, line);
}

// parse "<name> <size>" args. on success fills name (caller-owned, cap bytes)
// and *outSize, returns 1. on malformed input returns 0.
static int parseNameAndSize(const char *args, char *name, int cap, uint32_t *outSize)
{
    int i = 0;
    while (args[i] && args[i] != ' ') {
        if (i >= cap - 1) return 0;
        name[i] = args[i];
        i++;
    }
    if (i == 0 || args[i] != ' ') return 0;
    name[i] = '\0';
    i++;
    uint32_t v = 0;
    int digits = 0;
    while (args[i] >= '0' && args[i] <= '9') {
        v = v * 10 + (uint32_t)(args[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0) return 0;
    *outSize = v;
    return 1;
}

// extract a path token from args. supports "quoted paths" (any chars until
// closing ") or unquoted (until first space). on success fills out (caller-
// owned, cap bytes) and returns pointer to remainder (skipping trailing ws).
// on malformed input returns NULL.
static const char *parsePath(const char *args, char *out, int cap)
{
    int o = 0;
    if (*args == '"') {
        args++;
        while (*args && *args != '"') {
            if (o >= cap - 1) return 0;
            out[o++] = *args++;
        }
        if (*args != '"') return 0;
        args++;
    } else {
        while (*args && *args != ' ') {
            if (o >= cap - 1) return 0;
            out[o++] = *args++;
        }
    }
    if (o == 0) return 0;
    out[o] = '\0';
    while (*args == ' ') args++;
    return args;
}

// parse a non-negative decimal up to the next space or end. on success fills
// *outVal and returns pointer to remainder. on malformed input returns NULL.
static const char *parseUInt64(const char *args, uint64_t *outVal)
{
    uint64_t v = 0;
    int digits = 0;
    while (*args >= '0' && *args <= '9') {
        v = v * 10 + (uint64_t)(*args - '0');
        args++;
        digits++;
    }
    if (digits == 0) return 0;
    *outVal = v;
    while (*args == ' ') args++;
    return args;
}

// get-file <path> [offset length]
//   length=0 means "until end-of-file".
//   response: "OK <bytes>\n<bytes raw>" or "ERR <message>\n".
static void cmdGetFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    const char *rest = parsePath(args, path, sizeof path);
    if (!rest) {
        serverSendLine(cli, SDB_ERR " usage: get-file <path> [offset length]");
        return;
    }

    uint64_t offset = 0, length = 0;
    if (*rest) {
        rest = parseUInt64(rest, &offset);
        if (!rest) { serverSendLine(cli, SDB_ERR " bad offset"); return; }
        rest = parseUInt64(rest, &length);
        if (!rest) { serverSendLine(cli, SDB_ERR " bad length"); return; }
    }

    if (!fileExists(path)) {
        serverSendLine(cli, SDB_ERR " no such file");
        return;
    }
    int64_t window = fileWindowSize(path, offset, length);
    if (window < 0) {
        serverSendLine(cli, SDB_ERR " offset past end of file");
        return;
    }

    char header[64];
    snprintf(header, sizeof header, SDB_OK " %lld\n", (long long)window);
    if (serverSend(cli, header, (int)strLen(header)) < 0) return;

    if (window > 0 && sendFileWindow(cli, path, offset, (uint64_t)window) < 0) {
        // header already on the wire; client gets short read and reports it.
        dbgLog("[sdb] get-file send truncated\n");
    }
}

// save-file <path> <size>\n<size raw bytes>
//   response: "OK saved <path> (<size> bytes)" or "ERR ...".
//   does NOT auto-create parent directories — caller picks an existing path.
static void cmdSaveFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    const char *rest = parsePath(args, path, sizeof path);
    if (!rest) {
        serverSendLine(cli, SDB_ERR " usage: save-file <path> <size>");
        return;
    }
    uint64_t size = 0;
    rest = parseUInt64(rest, &size);
    if (!rest) {
        serverSendLine(cli, SDB_ERR " bad size");
        return;
    }
    if (recvFile(cli, path, (uint32_t)size) < 0) {
        serverSendLine(cli, SDB_ERR " save failed");
        return;
    }
    char reply[FILE_PATH_MAX + 64];
    snprintf(reply, sizeof reply, SDB_OK " saved %s (%llu bytes)", path, (unsigned long long)size);
    serverSendLine(cli, reply);
}

// delete-file <path>
//   response: "OK deleted <path>" or "ERR ...".
//   missing files are treated as success (idempotent).
static void cmdDeleteFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    if (!parsePath(args, path, sizeof path)) {
        serverSendLine(cli, SDB_ERR " usage: delete-file <path>");
        return;
    }
    if (deleteFile(path) < 0) {
        serverSendLine(cli, SDB_ERR " delete failed");
        return;
    }
    char reply[FILE_PATH_MAX + 64];
    snprintf(reply, sizeof reply, SDB_OK " deleted %s", path);
    serverSendLine(cli, reply);
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
    else if (serverMatchCmd(buf, "vsh-plugin-list")) {
        cmdVshPluginList(cli);
    }
    else {
        const char *args;
        char  name[PLUGIN_NAME_MAX];
        uint32_t size;
        char  reply[128];

        if ((args = serverMatchCmd(buf, "get-file")) != 0) {
            cmdGetFile(cli, args);
        }
        else if ((args = serverMatchCmd(buf, "save-file")) != 0) {
            cmdSaveFile(cli, args);
        }
        else if ((args = serverMatchCmd(buf, "delete-file")) != 0) {
            cmdDeleteFile(cli, args);
        }
        else if ((args = serverMatchCmd(buf, "vsh-plugin-install")) != 0) {
            if (!parseNameAndSize(args, name, sizeof name, &size)) {
                serverSendLine(cli, SDB_ERR " usage: vsh-plugin-install <name> <size>");
            } else if (pluginInstall(cli, name, size) < 0) {
                serverSendLine(cli, SDB_ERR " install failed");
            } else {
                snprintf(reply, sizeof reply, SDB_OK " installed %s (%u bytes)", name, (unsigned)size);
                serverSendLine(cli, reply);
            }
        }
        else if ((args = serverMatchCmd(buf, "vsh-plugin-uninstall")) != 0) {
            if (args[0] == '\0') {
                serverSendLine(cli, SDB_ERR " usage: vsh-plugin-uninstall <name>");
            } else if (pluginUninstall(args) < 0) {
                serverSendLine(cli, SDB_ERR " uninstall failed");
            } else {
                snprintf(reply, sizeof reply, SDB_OK " uninstalled %s", args);
                serverSendLine(cli, reply);
            }
        }
        else {
            serverSendLine(cli, SDB_ERR " unknown command");
        }
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
