// yo-player - a native PS3 YouTube client (in progress).
//
// Boilerplate mirrors file-manager (the proven full app): RTC + network up
// front, bridge logging, VSYNC-ON gfx, then the frame loop. It boots into the
// home screen (trending categories); searching / channels / playback stream via
// simple-lib-av. Bring-up diagnostics go to the bridge log (dbg.h).

#include <sys/process.h>

#include "app.h"
#include "vfs.h"
#include "http.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "screenshot.h"
#include "audio.h"
#include "font.h"
#include "screen-manager.h"
#include "screens/home.h"
#include "storage.h"
#include "settings.h"
#include "downloads.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"
#include "ui/stats.h"
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
   initRtc();
   initVfs();                    // file i/o routing (the temp download lands via openFs)

   int netRc = initNet();
   initModernHttp();   // bind the modern (BearSSL) http transport; all requests + media streams go through it
   registerWithBridge("app", "yo-player");

   if (initGfx(GFX_VSYNC_ON) != 0) return 1;
   if (initAudio() != 0) return 1;
   if (initFont() != 0) return 1;
   if (initIconFont() != 0) logError("[yt] embedded icon font failed to load; icons will be blank\n");
   initPad();
   enableScreenshot();

   initStats(5, 5, 14, COLOR_AMBER_300);
   initStorage();          // prefs (last category) + watch history, under /dev_hdd0/tmp/yo-player/
   loadSettings();         // user-editable settings.txt (created with defaults on first launch)
   loadConsoleGlyphs();    // decode the console's own button glyphs for the on-screen hints
   initDownloads();        // background download queue + its progress overlay

   logInfo("[yt] net rc=%d\n", netRc);
   openHome();       // boot into the home screen (trending categories)

   while (!appExitRequested) {
      appPoll();
      updatePad();
      handleScreenshot();
      updateScreen();

      beginGfxFrame();
      clearGfx(COLOR_SLATE_900);
      drawScreen();
      endGfxFrame();
   }

   changeScreen(NULL);
   shutdownDownloads();   // cancel + join any in-flight download before the http/vfs layers go away
   freeConsoleGlyphs();
   termStats();
   termAudio();
   termFont();
   termGfx();
   shutdownHttp();
   shutdownVfs();
   return 0;
}
