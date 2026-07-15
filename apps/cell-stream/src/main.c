#include <sys/process.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "audio.h"
#include "font.h"
#include "ui/label.h"
#include "dbg.h"
#include "thread.h"
#include "bridge-client.h"
#include "net-common.h"
#include "stream.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

// the stats panel, tucked into the top-left corner and out of the way
#define STAT_LINES     3
#define STAT_LINE_MAX  128
#define TEXT_LEFT      30
#define LINES_TOP      30
#define LINE_HEIGHT    30
#define TEXT_SIZE      20
#define PANEL_PADDING  10
#define PANEL_WIDTH    830
#define PANEL_BACKGROUND 0x70000000   // black, and see-through enough to read the picture behind it

#define STATUS_SIZE    42             // the "waiting for server ..." screen
#define STATUS_TOP     330
#define PAD_SEND_INTERVAL_US   4000   // 250Hz, the standard USB gamepad rate: a press waits ~2ms for its slot, not ~8ms
#define PAD_MODE_INTERVAL_US   1000000

// formats the stream stats into the panel's text lines (integer math only - no %f on PS3)
static void formatStreamLines(const StreamStats *streamStats, char lines[][STAT_LINE_MAX])
{
   snprintf(lines[0], STAT_LINE_MAX, "%dx%d  %dfps (source %d)  %dkbps", streamStats->width, streamStats->height,
            streamStats->receivedFps, streamStats->sourceFps, streamStats->bitrateKbps);
   snprintf(lines[1], STAT_LINE_MAX, "network %d.%d + decode %d.%d + present %d.%d + display %d.%d = %d.%dms to screen",
            streamStats->networkMsTenths / 10, streamStats->networkMsTenths % 10,
            streamStats->decodeMsTenths / 10, streamStats->decodeMsTenths % 10,
            streamStats->presentMsTenths / 10, streamStats->presentMsTenths % 10,
            streamStats->displayWaitMsTenths / 10, streamStats->displayWaitMsTenths % 10,
            streamStats->totalMsTenths / 10, streamStats->totalMsTenths % 10);
   snprintf(lines[2], STAT_LINE_MAX, "lost %d  behind %d  skipped %d  pipeline %d  decode floor %d.%dms",
            streamStats->framesIncomplete, streamStats->framesDroppedBehind, streamStats->framesSkipped,
            streamStats->pipelineDepth, streamStats->decodeMinMsTenths / 10, streamStats->decodeMinMsTenths % 10);
}

// while streaming every button belongs to the PC - a game needs all of them. so the app keeps nothing
// for itself and uses SELECT as a modifier instead: the combos below. the buttons of a combo are held
// back from the PC, so the game never sees a stray press when one is used.
#define PAD_BIT(button) (1u << (button))

static void sendPadStateToServer(unsigned heldBack)
{
   unsigned buttons = 0;
   int button;
   for (button = PAD_BTN_UP; button <= PAD_BTN_R3; button++)
      if (isPadButtonDown((PadButton)button)) buttons |= PAD_BIT(button);
   buttons &= ~heldBack;

   Stick left = getPadLeftStick(), right = getPadRightStick();
   sendPadState(buttons, left.x, left.y, right.x, right.y);
}

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   initRtc();
   appRegisterExitCallback();

   int netRc = initNet();
   logInfo("[cst] initNet rc=0x%x\n", netRc);
   registerWithBridge("app", "cell-stream");   // live logs in the bridge client's Logs tab

   // a decoded picture goes to the screen the moment it is ready, never waiting for the display's next
   // refresh - that wait costs ~8ms and buys us nothing here (measured; see the README)
   if (initGfx(GFX_VSYNC_OFF) != 0) return 1;
   if (initFont() != 0) return 1;
   initPad();
   int audioRc = initAudio();   // the mixer's PCM feed carries the PC's sound
   logInfo("[cst] initAudio rc=0x%x\n", audioRc);
   initStream();   // finds the server and keeps a stream up by itself, for as long as the app runs

   Font font = openSystemFont(FONT_SANS);

   // shown alone on a black screen whenever no video is arriving: "waiting for server ...", etc
   Label status;
   char shownStatus[STAT_LINE_MAX] = "";
   initLabelRaw(&status, &font, 0, STATUS_TOP, AUTO, AUTO, STATUS_SIZE, COLOR_WHITE, TEXT_NOWRAP, "");

   Label lines[STAT_LINES];
   char shownText[STAT_LINES][STAT_LINE_MAX];
   int i;
   for (i = 0; i < STAT_LINES; i++) {
      shownText[i][0] = 0;
      initLabelRaw(&lines[i], &font, TEXT_LEFT, LINES_TOP + i * LINE_HEIGHT, PANEL_WIDTH, AUTO, TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, "");
   }

   int exitRequested = 0, statsVisible = 0, gamepadMode = 1;   // stats overlay off by default; SELECT+L3 shows it
   int wasLive = 0, framesUntilBufferRelease = 0;
   uint64_t lastPadSendUs = 0, lastPadModeSendUs = 0;
   while (!appExitRequested && !exitRequested) {
      appPoll();
      updatePad();

      int live = isStreamLive();

      // free an ended stream's video memory only after a few video-less frames have rendered, so the
      // RSX is guaranteed done with it (freeing straight away hard-froze the app). the next session
      // waits for this before allocating its own.
      if (wasLive && !live) framesUntilBufferRelease = 4;
      if (!live && framesUntilBufferRelease > 0 && --framesUntilBufferRelease == 0) releaseStreamBuffers();
      wasLive = live;

      unsigned heldBack = 0;
      if (live) {
         // every button belongs to the PC while streaming, so SELECT is the modifier for ours
         if (isPadButtonDown(PAD_BTN_SELECT)) {
            heldBack = PAD_BIT(PAD_BTN_SELECT) | PAD_BIT(PAD_BTN_R3) | PAD_BIT(PAD_BTN_L3);
            if (isPadButtonPressed(PAD_BTN_R3)) { gamepadMode = !gamepadMode; lastPadModeSendUs = 0; }
            if (isPadButtonPressed(PAD_BTN_L3)) statsVisible = !statsVisible;
         }
      } else {
         if (isPadButtonPressed(PAD_BTN_START)) exitRequested = 1;
         if (isPadButtonPressed(PAD_BTN_L3)) statsVisible = !statsVisible;
      }

      // the pad goes out on its own 250Hz clock now that the loop no longer runs at the display's rate
      uint64_t nowUs = getTimeUs();
      if (live && nowUs - lastPadSendUs >= PAD_SEND_INTERVAL_US) {
         lastPadSendUs = nowUs;
         // the mode is repeated once a second: the first one can go out before the pad socket exists,
         // and it is a lone UDP packet, so a lost one would otherwise leave the PC on the wrong device
         if (nowUs - lastPadModeSendUs >= PAD_MODE_INTERVAL_US) {
            lastPadModeSendUs = nowUs;
            sendPadMode(gamepadMode);
         }
         sendPadStateToServer(heldBack);
      }

      // streaming, and no new picture: don't redraw the same one, just keep the loop cheap
      if (live && !isNewStreamPictureReady()) { sleepMs(1); continue; }
      if (!live) sleepMs(16);   // no video to pace us: don't spin the waiting screen flat out

      StreamStats streamStats;
      getStreamStats(&streamStats);

      if (live) {   // re-render only the stat lines that changed
         char current[STAT_LINES][STAT_LINE_MAX];
         formatStreamLines(&streamStats, current);
         for (i = 0; i < STAT_LINES; i++) {
            if (strcmp(current[i], shownText[i]) != 0) {
               strcpy(shownText[i], current[i]);
               setLabelText(&lines[i], current[i]);
            }
         }
      }

      beginGfxFrame();
      clearGfx(COLOR_BLACK);
      if (live) {
         drawStreamFrame();
      } else if (strcmp(streamStats.error, shownStatus) != 0 || shownStatus[0]) {
         // no server: say so, big, in the middle of an otherwise empty screen
         if (strcmp(streamStats.error, shownStatus) != 0) {
            strcpy(shownStatus, streamStats.error);
            setLabelText(&status, shownStatus);
            status.x = (getGfxScreenWidth() - (int)measureFontText(&font, STATUS_SIZE, shownStatus)) / 2;
         }
         drawLabel(&status);
      }
      if (statsVisible && live) {
         // a dark panel behind the text so it stays readable over any picture
         int panelHeight = STAT_LINES * LINE_HEIGHT + PANEL_PADDING * 2;
         fillGfxRectangle(TEXT_LEFT - PANEL_PADDING, LINES_TOP - PANEL_PADDING, PANEL_WIDTH, panelHeight, PANEL_BACKGROUND);
         for (i = 0; i < STAT_LINES; i++) drawLabel(&lines[i]);
      }
      endGfxFrame();
      if (live) noteStreamFlipWait(getGfxFlipWaitUs());   // what waiting for the display cost us
   }

   stopStream();
   while (isStreamRunning()) sleepMs(10);   // let the connect loop shut the decoder down cleanly
   for (i = 0; i < 4; i++) {   // flush video-less frames through the RSX before freeing its buffers
      beginGfxFrame();
      clearGfx(COLOR_BLACK);
      endGfxFrame();
   }
   releaseStreamBuffers();

   freeLabel(&status);
   for (i = 0; i < STAT_LINES; i++) freeLabel(&lines[i]);
   closeFont(&font);
   termFont();
   termAudio();
   termGfx();
   return 0;
}
