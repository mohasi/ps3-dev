#pragma once

// app - exit handling via sysutil callback, network bring-up

#include <sysutil/sysutil_common.h>
#include <cell/sysmodule.h>
#include <netex/net.h>
#include <stddef.h>

static volatile int appExitRequested = 0;

static void appExitCallback(uint64_t status, uint64_t param, void *userdata)
{
    (void)param; (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME) appExitRequested = 1;
}

static inline void appRegisterExitCallback(void)
{
    cellSysutilRegisterCallback(0, appExitCallback, NULL);
}

static inline void appPoll(void)
{
    cellSysutilCheckCallback();
}

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
