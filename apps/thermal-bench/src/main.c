#include <sys/process.h>

#include "app.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "font.h"
#include "screen-manager.h"
#include "ui/console-glyphs.h"
#include "screens/bench.h"
#include "bridge-client.h"
#include "settings.h"
#include "dbg.h"
#include "vfs.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   initRtc();
   initVfs();          // dbg.h logging and the run files both write through vfs
   logBuildVersion();
   appRegisterExitCallback();

   initNet();
   registerWithBridge("app", "thermal-bench");   // live logs in the bridge client's Logs tab

   loadSettings();     // safety cutoff, defaulted from the console model (settings.txt, created on first launch)

   if (initGfx(GFX_VSYNC_ON) != 0) return 1;
   if (initFont() != 0) return 1;
   if (loadConsoleGlyphs() != 0) logWarn("[bench] button glyphs unavailable; hints will show captions only\n");
   initPad();

   changeScreen(&benchScreen);

   while (!appExitRequested) {
      appPoll();
      updatePad();
      updateScreen();

      beginGfxFrame();
      clearGfx(COLOR_SLATE_900);
      drawScreen();
      endGfxFrame();
   }

   changeScreen(NULL);
   shutdownVfs();
   freeConsoleGlyphs();
   termFont();
   termGfx();
   return 0;
}
