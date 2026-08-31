// Simple Cheat Menu — VSH plugin entry point.
//
// An in-game overlay listing cheats for the running title, with on/off toggles that
// patch and restore game memory live via cobra. The pieces:
//   trigger.c        — the PS-button trigger + capture state machine (owns the threads)
//   overlay.cpp      — the PAF overlay: packed cheat storage + rendering + apply/revert
//   overlay-bridge.c — the C<->C++ glue overlay.cpp needs (logging + lv2 heap)
//   game-mem.c       — game-process memory read/write (ps3mapi)
// This file is just the module entry: register with the bridge and start the threads.

#include <sys/prx.h>

#include "dbg.h"
#include "bridge-client.h"
#include "trigger.h"
#include "thread.h"   // exitLoaderThread

SYS_MODULE_INFO(SimpleCheatMenu, 0, 1, 1);
SYS_MODULE_START(_start);

#define TAG "[cht] "

int _start(uint64_t arg)
{
   (void)arg;
   registerWithBridge("plugin", "cht");
   logInfo(TAG "_start\n");
   logBuildVersion();
   startCheatMenuThreads();   // trigger + worker threads; _start must return promptly
   exitLoaderThread();
   return SYS_PRX_RESIDENT;   // unreachable
}
