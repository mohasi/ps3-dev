#pragma once

// app - exit handling via sysutil callback

#include <sysutil/sysutil_common.h>
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
