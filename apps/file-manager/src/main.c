#include <sys/process.h>

#include "app.h"
#include "vfs.h"
#include "syscall.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "screenshot.h"
#include "audio.h"
#include "font.h"
#include "screen-manager.h"
#include "screens/home.h"
#include "theme.h"
#include "ui/stats.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"
#include "ui/checkbox.h"
#include "dbg.h"
#include "bridge-client.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   appRegisterExitCallback();
   initRtc();
   mountDevBlind();
   initVfs();
   initThemes();   // built-in + themes.txt palettes; must precede any screen that reads activeTheme

   initNet();
   registerWithBridge("app", "file-manager");

   if (initGfx(GFX_VSYNC_ON) != 0) return 1;
   if (initAudio() != 0) return 1;
   if (initFont() != 0) return 1;
   initPad();
   enableScreenshot();
   loadConsoleGlyphs();   // decode the console's own button glyphs for footer/keyboard hints
   if (initIconFont() != 0) logError("[icons] embedded icon font failed to load; icons will be blank\n");
   initCheckboxIcons();   // rasterise the checkbox box/check glyphs once

   initStats(5, 5, 14, COLOR_AMBER_300);

   changeScreen(&homeScreen);

   while (!appExitRequested) {
      appPoll();
      updatePad();
      handleScreenshot();
      updateScreen();

      beginGfxFrame();
      drawScreen();
      endGfxFrame();
   }

   changeScreen(NULL);
   termStats();
   freeConsoleGlyphs();
   freeCheckboxIcons();
   freeIconFont();
   termAudio();
   termFont();
   termGfx();
   shutdownVfs();
   return 0;
}
