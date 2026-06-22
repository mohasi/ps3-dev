#include <sys/process.h>

#include "app.h"
#include "vfs.h"
#include "syscall.h"   // mountDevBlind
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "screen-manager.h"
#include "screens/home.h"
#include "bridge-client.h"
#include "dbg.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   appRegisterExitCallback();
   mountDevBlind();

   initRtc();
   initNet();
   registerWithBridge("app", "rpp");

   logInfo("[rpp] starting\n");

   if (initGfx(GFX_VSYNC_ON) != 0) { logError("[rpp] initGfx failed\n"); return 1; }
   if (initFont() != 0) { logError("[rpp] initFont failed\n"); return 1; }
   initPad();

   changeScreen(&homeScreen);

   while (!appExitRequested) {
      appPoll();
      updatePad();
      updateScreen();

      beginGfxFrame();
      drawScreen();
      endGfxFrame();
   }

   changeScreen(NULL);
   termFont();
   termGfx();
   logInfo("[rpp] shutdown\n");
   return 0;
}
