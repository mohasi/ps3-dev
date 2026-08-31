// simple-debug-bridge — VSH plugin
//
// TCP listener on port 8785. host clients send framed commands; producer
// plugins/apps connect on the same port and forward their log lines. full
// command surface is the dispatch table in server.h.
//
// The VFS (for exFAT/NTFS access from file commands) is brought up by the server
// thread once XMB is ready (see startServer in server.h); the VFS owns its own
// 8 KB hotplug poll thread, so nothing here drives it. _stop tears it down.

#include <sys/prx.h>

#include "dbg.h"
#include "syscall.h"
#include "server.h"
#include "thread.h"   // exitLoaderThread

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
   logBuildVersion();
   startServer();
   setLogCallback(forwardLogToHost);
   exitLoaderThread();
   return SYS_PRX_RESIDENT;   // unreachable
}

int _stop(void)
{
   logInfo("[sdb] _stop\n");
   stopServer();      // joins the connection threads first
   prxFinalizeSelf();
   exitLoaderThread();
   return SYS_PRX_STOP_OK;   // unreachable
}
