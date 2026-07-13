// app - see app.h
#include "app.h"

volatile int appExitRequested = 0;

static void appExitCallback(uint64_t status, uint64_t param, void *userdata)
{
   (void)param; (void)userdata;
   if (status == CELL_SYSUTIL_REQUEST_EXITGAME) appExitRequested = 1;
}

void appRegisterExitCallback(void) { cellSysutilRegisterCallback(0, appExitCallback, NULL); }
void appPoll(void) { cellSysutilCheckCallback(); }
void requestAppExit(void) { appExitRequested = 1; }
