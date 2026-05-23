// simple-debug-bridge — VSH plugin
//
// TCP listener on port 8785. host clients send framed commands; producer
// plugins/apps connect on the same port and forward their log lines. full
// command surface is the dispatch table in server.h.

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
    // _start runs before the callback is installed, so this line is dbg.txt-only.
    // everything after setLogCallback is tee'd to the host via forwardLogToHost,
    // with pre-connect lines buffered in the server-side ring.
    logInfo("[sdb] _start\n");
    startServer();
    setLogCallback(forwardLogToHost);
    return SYS_PRX_RESIDENT;
}

int _stop(void)
{
    logInfo("[sdb] _stop\n");
    stopServer();
    prxFinalizeSelf();
    return SYS_PRX_STOP_OK;
}
