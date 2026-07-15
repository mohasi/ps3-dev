// screenshot - implementation. See screenshot.h.
#include "screenshot.h"

#include <cell/rtc.h>   // CellRtcDateTime, cellRtcGetCurrentClockLocalTime

#include "gfx.h"            // getGfxDisplayBuffer
#include "pad.h"            // L3/R3 state
#include "image-loader.h"   // savePngArgbPitch
#include "vfs.h"            // makeDirPath
#include "printf.h"         // snprintf
#include "dbg.h"            // logInfo / logError

#define SCREENSHOT_DIR "/dev_hdd0/tmp/screenshots"

static int screenshotEnabled = 0;

void enableScreenshot(void)
{
   screenshotEnabled = 1;
}

int takeScreenshot(char *outPath, int cap)
{
   int width = 0, height = 0, pitch = 0;
   const void *front = getGfxDisplayBuffer(&width, &height, &pitch);
   if (!front || width <= 0 || height <= 0 || pitch <= 0) { logError("[shot] no front buffer\n"); return -1; }

   CellRtcDateTime now;
   if (cellRtcGetCurrentClockLocalTime(&now) != 0) { logError("[shot] clock read failed\n"); return -1; }

   makeDirPath(SCREENSHOT_DIR);

   char path[256];
   snprintf(path, sizeof path, "%s/%04d%02d%02d-%02d%02d%02d.png",
            SCREENSHOT_DIR, now.year, now.month, now.day, now.hour, now.minute, now.second);

   int rc = savePngArgbPitch(path, front, width, height, pitch);
   if (rc != 0) { logError("[shot] encode failed rc=%d\n", rc); return rc; }

   logInfo("[shot] saved %s (%dx%d)\n", path, width, height);
   if (outPath) snprintf(outPath, cap, "%s", path);
   return 0;
}

int handleScreenshot(void)
{
   if (!screenshotEnabled) return 0;

   // fire once, on the frame L3+R3 first becomes fully held: both down, and at least one only just
   // pressed this frame (so holding the combo doesn't spray screenshots every frame).
   int bothDown = isPadButtonDown(PAD_BTN_L3) && isPadButtonDown(PAD_BTN_R3);
   int justCompleted = isPadButtonPressed(PAD_BTN_L3) || isPadButtonPressed(PAD_BTN_R3);
   if (bothDown && justCompleted) return takeScreenshot(0, 0) == 0;
   return 0;
}
