#pragma once

// app - exit handling via sysutil callback, network bring-up

#include <sysutil/sysutil_common.h>
#include <cell/sysmodule.h>
#include <netex/net.h>
#include <stddef.h>

// set once the system asks the app to quit (XMB exit) or requestAppExit() is called; the main
// loop runs until it's set. Lives in app.c so every file sees the same flag - as a header
// variable each file silently got its own private copy.
extern volatile int appExitRequested;

// set while the XMB is open over the app (PS button). the app keeps running and
// must keep calling appPoll, but anything that hogs the machine - busy worker
// threads, uncapped rendering - has to back off or the XMB cannot draw and the
// console looks hung.
extern volatile int appSystemMenuOpen;

void appRegisterExitCallback(void);
void appPoll(void);
void requestAppExit(void);   // exit from app code (e.g. a quit menu), same path as a system exit

// load the NET sysmodule and bring up the socket stack. apps run as cold
// NPDRM processes so the network APIs are not live until this runs; VSH
// plugins skip this because VSH has already initialized the stack.
// returns the lv2 rc from sys_net_initialize_network (0 on success).
static inline int initNet(void)
{
   cellSysmoduleLoadModule(CELL_SYSMODULE_NET);
   return sys_net_initialize_network();
}

// load RTC for app-side timestamped logging in dbg.h.
static inline int initRtc(void)
{
   return cellSysmoduleLoadModule(CELL_SYSMODULE_RTC);
}
