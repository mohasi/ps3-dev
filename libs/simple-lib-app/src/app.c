// app - see app.h
#include "app.h"

volatile int appExitRequested = 0;
volatile int appSystemMenuOpen = 0;

static void appSystemCallback(uint64_t status, uint64_t param, void *userdata)
{
   (void)param; (void)userdata;
   switch (status)
   {
      case CELL_SYSUTIL_REQUEST_EXITGAME:    appExitRequested = 1;  break;
      case CELL_SYSUTIL_SYSTEM_MENU_OPEN:    appSystemMenuOpen = 1; break;
      case CELL_SYSUTIL_SYSTEM_MENU_CLOSE:   appSystemMenuOpen = 0; break;
      default: break;
   }
}

void appRegisterExitCallback(void) { cellSysutilRegisterCallback(0, appSystemCallback, NULL); }
void appPoll(void) { cellSysutilCheckCallback(); }
void requestAppExit(void) { appExitRequested = 1; }
