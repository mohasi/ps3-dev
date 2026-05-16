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
    logInfo("[sdb] _start\n");
    serverStart();
    return SYS_PRX_RESIDENT;
}

int _stop(void)
{
    logInfo("[sdb] _stop\n");
    serverStop();
    prxFinalizeSelf();
    return SYS_PRX_STOP_OK;
}
