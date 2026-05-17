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
 * for registration — the registration thread runs in parallel.
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

#define BRIDGE_LOOPBACK_PORT   8785
#define BRIDGE_CONNECT_TRIES   150            // 150 * 200ms = 30s ceiling
#define BRIDGE_RETRY_USEC      (200 * 1000)

// pre-connect line backlog. logInfo/Warn/Error fired during plugin startup
// (xmb-ready wait, mounts, listener-retry races) happens before the TCP
// connect to the bridge completes; without this ring those lines never
// reach the host's Logs tab. drained on successful REGISTER.
#define BRIDGE_BACKLOG 64

static BacklogLine bridgeBacklog[BRIDGE_BACKLOG];
static int bridgeBacklogHead  = 0;
static int bridgeBacklogCount = 0;

typedef struct {
    const char *kind;
    const char *name;
} BridgeRegArgs;

static int bridgeFd = -1;

// send "<verb> <n>\n<n bytes>" over the bridge socket. drops the socket on
// any failure so subsequent calls are no-ops until something reconnects.
static inline void sendToBridge(const char *verb, const void *buf, int len)
{
    if (bridgeFd < 0) return;

    char hdr[32];
    int hLen = snprintf(hdr, sizeof hdr, "%s %d\n", verb, len);

    const char *p = hdr;
    int remaining = hLen;
    while (remaining > 0) {
        int n = send(bridgeFd, p, remaining, 0);
        if (n <= 0) { socketclose(bridgeFd); bridgeFd = -1; return; }
        p += n; remaining -= n;
    }
    p = (const char *)buf;
    remaining = len;
    while (remaining > 0) {
        int n = send(bridgeFd, p, remaining, 0);
        if (n <= 0) { socketclose(bridgeFd); bridgeFd = -1; return; }
        p += n; remaining -= n;
    }
}

// log sink installed by registerWithBridge — fires AFTER the dbg.txt
// write so disk remains the source of truth. while the bridge socket isn't
// up yet, queue into the local ring; once it is, push live.
static inline void pushLogToBridge(const char *line, int len)
{
    if (bridgeFd < 0) {
        int take = len < LOG_LINE_MAX ? len : LOG_LINE_MAX;
        BacklogLine *slot = &bridgeBacklog[bridgeBacklogHead];
        slot->len = take;
        for (int i = 0; i < take; i++) slot->data[i] = line[i];
        bridgeBacklogHead = (bridgeBacklogHead + 1) % BRIDGE_BACKLOG;
        if (bridgeBacklogCount < BRIDGE_BACKLOG) bridgeBacklogCount++;
        return;
    }
    sendToBridge("LOG", line, len);
}

// drain pre-connect lines in chronological order. called right after the
// REGISTER handshake succeeds, before any live log can race in (the sink
// is already installed but bridgeFd was -1 until we set it).
static inline void drainBacklog(void)
{
    if (bridgeBacklogCount == 0) return;
    int start = (bridgeBacklogHead - bridgeBacklogCount + BRIDGE_BACKLOG) % BRIDGE_BACKLOG;
    for (int i = 0; i < bridgeBacklogCount; i++) {
        BacklogLine *slot = &bridgeBacklog[(start + i) % BRIDGE_BACKLOG];
        sendToBridge("LOG", slot->data, slot->len);
    }
    bridgeBacklogCount = 0;
    bridgeBacklogHead  = 0;
}

// background thread: retries the connect until the bridge accepts (up to
// BRIDGE_CONNECT_TRIES), sends the REGISTER handshake, and installs the
// log sink. exits silently on timeout — caller's logs keep flowing to
// dbg.txt only, which is acceptable for early-startup errors per design.
static void runBridgeRegistration(uint64_t arg)
{
    BridgeRegArgs *a = (BridgeRegArgs *)(uintptr_t)arg;

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(BRIDGE_LOOPBACK_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int s = -1;
    for (int i = 0; i < BRIDGE_CONNECT_TRIES; i++) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0 && connect(s, (struct sockaddr *)&addr, sizeof addr) == 0) break;
        if (s >= 0) { socketclose(s); s = -1; }
        sys_timer_usleep(BRIDGE_RETRY_USEC);
    }
    if (s < 0) { sys_ppu_thread_exit(0); return; }

    char hs[128];
    int hsLen = snprintf(hs, sizeof hs, "REGISTER %s %s\n", a->kind, a->name);
    const char *p = hs;
    int remaining = hsLen;
    while (remaining > 0) {
        int n = send(s, p, remaining, 0);
        if (n <= 0) { socketclose(s); sys_ppu_thread_exit(0); return; }
        p += n; remaining -= n;
    }

    bridgeFd = s;
    drainBacklog();
    // first line emitted through the sink — confirms registration end-to-end
    // and gives the host a guaranteed marker even for silent plugins.
    logInfo("[%s] bridge link up\n", a->name);
    sys_ppu_thread_exit(0);
}

// fire-and-forget: spawn the registration thread and return immediately.
// caller-owned strings must outlive the thread (literals are fine). the
// sink is installed synchronously so any logs emitted before (or during)
// the TCP connect get queued for replay after REGISTER.
static inline void registerWithBridge(const char *kind, const char *name)
{
    if (bridgeFd >= 0) return;

    setLogSink(pushLogToBridge);

    static BridgeRegArgs args;   // single-shot per plugin
    args.kind = kind;
    args.name = name;

    sys_ppu_thread_t tid = 0;
    spawnThread(&tid, runBridgeRegistration, (uint64_t)(uintptr_t)&args, THREAD_STACK_SIZE_8KB, "sdb_reg");
}
