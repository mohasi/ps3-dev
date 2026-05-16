#pragma once

/* HTTP listener that intercepts XMB menu clicks and mounts ISOs via Cobra.
 * webrender_plugin GETs http://127.0.0.1:8947/mount/<full-path> when the
 * user presses X on an ISO item. */

#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "dbg.h"
#include "file.h"
#include "cobra.h"
#include "vsh.h"
#include "string-utilities.h"

#define SDM_PORT      8947
#define SDM_BUF_MAX   2048
#define SDM_PATH_MAX  1024

static const char *pathLastMount = "/dev_hdd0/tmp/sdm_last.txt";

/* Tiny HTML page that closes the webrender browser as soon as it loads.
 * Sent on every served request — success and bail-out paths alike — so the
 * page never dangles in front of the user. Content-Length must match the
 * body exactly or some clients hang waiting for more bytes. */
static const char httpRespAutoClose[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: 31\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<script>window.close()</script>";

static void httpWriteLastMount(const char *path)
{
    writeFile(pathLastMount, path, (uint64_t)strLen(path));
}

/* Parses the request in `buf` and, if it's a well-formed mount URL whose
 * target exists, fires cobraMountIso. All log/notify side-effects live here.
 * Returns nothing — caller always sends the auto-close response so the
 * webrender browser never dangles, regardless of which bail path we take. */
static void httpParseAndMount(const char *buf, int off)
{
    static const char prefix[] = "GET /mount/";
    int prefixLen = (int)sizeof(prefix) - 1;
    if (off < prefixLen) return;
    for (int i = 0; i < prefixLen; i++) {
        if (buf[i] != prefix[i]) return;
    }

    char path[SDM_PATH_MAX];
    if (urlDecode(buf + prefixLen, path, sizeof path) <= 0) {
        logError("[sdm] url decode failed\n");
        return;
    }
    if (!fileExists(path)) {
        logError("[sdm] file not found: %s\n", path);
        return;
    }

    if (cobraMountIso(path) == SUCCESS) {
        httpWriteLastMount(path);
        logInfo("[sdm] mounted: %s\n", path);
        vshNotify("Disc mounted.");
    } else {
        logError("[sdm] mount failed: %s\n", path);
    }
}

static void httpHandle(int cli)
{
    char buf[SDM_BUF_MAX];
    int off = 0;
    while (off < (int)sizeof(buf) - 1) {
        int n = recv(cli, buf + off, sizeof(buf) - 1 - off, 0);
        if (n <= 0) return;     /* dead socket — no point trying to respond */
        off += n;
        buf[off] = '\0';
        if (off >= 4 && buf[off-4] == '\r' && buf[off-3] == '\n' &&
            buf[off-2] == '\r' && buf[off-1] == '\n') break;
    }

    httpParseAndMount(buf, off);
    send(cli, httpRespAutoClose, sizeof(httpRespAutoClose) - 1, 0);
}

static void httpListenerThread(uint64_t arg)
{
    (void)arg;
    logInfo("[sdm] http thread start\n");

    int fd = -1;
    int retries = 0;
    while (fd < 0 && retries < 30) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { sys_timer_sleep(2); retries++; }
    }
    if (fd < 0) { logError("[sdm] socket failed\n"); sys_ppu_thread_exit(0); return; }

    struct sockaddr_in a;
    a.sin_family      = AF_INET;
    a.sin_port        = htons(SDM_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    retries = 0;
    while (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        if (++retries > 30) {
            logError("[sdm] bind failed\n");
            socketclose(fd);
            sys_ppu_thread_exit(0);
            return;
        }

        sys_timer_sleep(2);
    }
    if (listen(fd, 2) < 0) {
        logError("[sdm] listen failed\n");
        socketclose(fd);
        sys_ppu_thread_exit(0);
        return;
    }

    logInfo("[sdm] listening on :%d\n", SDM_PORT);

    for (;;) {
        socklen_t rl = sizeof a;
        int c = accept(fd, (struct sockaddr *)&a, &rl);
        if (c < 0) { sys_timer_usleep(100000); continue; }
        httpHandle(c);
        shutdown(c, SHUT_RDWR);
        socketclose(c);
    }
}

