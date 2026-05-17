// simple-debug-bridge — VSH plugin
//
// opens a TCP listener on port 8785 (LAN) so a PC client can send
// commands: restart, shutdown, screenshot.

#include <sys/prx.h>

#include "dbg.h"
#include "syscall.h"
#include "server.h"

SYS_MODULE_INFO(SimpleDebugBridge, 0, 1, 0);
SYS_MODULE_START(_start);
SYS_MODULE_STOP(_stop);

int _start(uint64_t arg)
{
    (void)arg;
    // _start runs before the sink is installed, so this line is dbg.txt-only.
    // everything after setLogSink is tee'd to the host via forwardLogToHost,
    // with pre-connect lines buffered in the server-side ring.
    logInfo("[sdb] _start\n");
    startServer();
    setLogSink(forwardLogToHost);
    return SYS_PRX_RESIDENT;
}

int _stop(void)
{
    logInfo("[sdb] _stop\n");
    stopServer();
    prxFinalizeSelf();
    return SYS_PRX_STOP_OK;
}
