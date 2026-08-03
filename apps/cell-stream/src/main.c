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
#include "shortcuts.h"
#include "toast.h"
#include "vfs.h"
#include "ui/keyboard.h"
#include "companion-prompt.h"
#include "shortcut-hint.h"
#include "frametime-graph.h"
#include "frame-timing.h"
#include "cell-stream-settings.h"
#include "settings-file.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

// the stats panel, tucked into the top-left corner: a vertical "Label: value" list above the
// frame-time graph and a "[SELECT][stats] Hide" footer, all inside one bordered panel
#define STAT_LINES       11
#define STAT_VALUE_MAX   32
#define STATS_X          30
#define STATS_Y          30
#define STATS_PADDING    14
#define STAT_TEXT_SIZE   14
#define STAT_LINE_STEP   19
#define STAT_VALUE_X     100          // value column offset from the label column (values left-aligned here)
#define GRAPH_WIDTH      220
#define STATS_GRAPH_GAP  14           // between the text block and the graph
#define STATS_HINT_GAP   12           // between the graph and the Hide footer
#define PANEL_BACKGROUND 0x96000000   // fairly transparent so the picture still reads through
#define PANEL_BORDER     0xFF3A3A3A

static const char *statFieldNames[STAT_LINES] = {
   "Resolution:", "Framerate:", "Bitrate:", "Latency:", "Network:", "Decode:",
   "Present:", "Display:", "Pipeline:", "Lost:", "Behind:"
};

#define PAD_SEND_INTERVAL_US   4000   // 250Hz, the standard USB gamepad rate: a press waits ~2ms for its slot, not ~8ms
#define PAD_MODE_INTERVAL_US   1000000

// what the pad drives on the PC. SELECT+input-mode cycles through these; controller forwards a virtual
// gamepad, mouse+keyboard forwards the mouse and can raise the on-screen keyboard on demand (Triangle).
// the PC only cares gamepad-vs-mouse.
typedef enum { INPUT_MOUSE_KEYBOARD, INPUT_CONTROLLER, INPUT_MODE_COUNT } InputMode;
static const char *inputModeNames[INPUT_MODE_COUNT] = { "Mouse + keyboard", "Controller" };
static const char *inputModeTokens[INPUT_MODE_COUNT] = { "mouse+keyboard", "controller" };  // settings.txt values

// the stream modes cycled with SELECT+Square - all 720p/60, differing only in how the PS3 presents the
// picture: vsync off shows each frame the instant it decodes (lowest delay, a little tearing); vsync locks
// to the refresh (no tearing); the buffer adds a one-frame reserve on top of vsync to ride out late frames.
typedef struct {
   const char *name;    // shown in the toast
   const char *token;   // saved in settings.txt
   int vsyncOn;
   int buffered;
} StreamMode;

static const StreamMode streamModes[] = {
   { "720p/60 vsync off",                "720p60-vsync-off", 0, 0 },
   { "720p/60 vsync",                    "720p60-vsync",     1, 0 },
   { "720p/60 vsync + one-frame buffer", "720p60-buffer",    1, 1 },
};
#define STREAM_MODE_COUNT   ((int)(sizeof streamModes / sizeof streamModes[0]))
#define DEFAULT_STREAM_MODE 1   // 720p/60 vsync - perceived as the most fluid

#define KEY_INPUT_MODE     "saved-input-mode"
#define KEY_STREAMING_MODE "saved-streaming-mode"

// picks the enum whose token matches the saved value, or `fallback` when the key is absent or unknown
static int loadEnumSetting(const char *text, const char *key, const char *const *tokens, int count, int fallback)
{
   const char *value = findSettingValue(text, key);
   if (value)
      for (int i = 0; i < count; i++)
         if (settingValueEquals(value, tokens[i])) return i;
   return fallback;
}

static void saveEnumSetting(const char *key, const char *token)
{
   upsertSettingValue(CELL_STREAM_SETTINGS_PATH, key, token);
}

// the stream mode's tokens live in the streamModes table, so match against those rather than a flat list
static int loadStreamMode(const char *text)
{
   const char *value = findSettingValue(text, KEY_STREAMING_MODE);
   if (value)
      for (int i = 0; i < STREAM_MODE_COUNT; i++)
         if (settingValueEquals(value, streamModes[i].token)) return i;
   return DEFAULT_STREAM_MODE;
}

// the on-screen keyboard's dark, translucent look (the lib is theme-agnostic)
static const KeyGridTheme keyboardTheme = {
   .panelFill = 0xC0000000, .panelBorder = 0xFF3A3A3A,
   .keyHighlightFill = 0xFF2A6FF0, .keyHighlightBorder = 0xFFFFFFFF,
   .keyText = 0xFFFFFFFF, .borderThickness = 2
};

// the keyboard owns these buttons while it is open, so hold them back from the PC
#define KEYBOARD_HELD_BACK (PAD_BIT(PAD_BTN_UP) | PAD_BIT(PAD_BTN_DOWN) | PAD_BIT(PAD_BTN_LEFT) | \
   PAD_BIT(PAD_BTN_RIGHT) | PAD_BIT(PAD_BTN_CROSS) | PAD_BIT(PAD_BTN_CIRCLE) | \
   PAD_BIT(PAD_BTN_SQUARE) | PAD_BIT(PAD_BTN_TRIANGLE))

static void onKeyboardKey(char key) { sendKeystroke(key); }

// applies a mode: vsync on/off and whether to hold a one-frame buffer. all modes are 720p/60, so this only
// changes how the picture is presented - no reconnect, so cycling among them is seamless.
static void applyStreamMode(int index)
{
   const StreamMode *mode = &streamModes[index];
   setGfxVsync(mode->vsyncOn ? GFX_VSYNC_ON : GFX_VSYNC_OFF);
   setStreamBuffered(mode->buffered);
}

// formats each stat into its value string, in statFieldNames order (integer math only - no %f on PS3)
static void formatStatValues(const StreamStats *s, char values[][STAT_VALUE_MAX])
{
   snprintf(values[0], STAT_VALUE_MAX, "%dx%d", s->width, s->height);
   snprintf(values[1], STAT_VALUE_MAX, "%d / %d fps", s->receivedFps, s->sourceFps);
   snprintf(values[2], STAT_VALUE_MAX, "%d kbps", s->bitrateKbps);
   snprintf(values[3], STAT_VALUE_MAX, "%d.%d ms", s->totalMsTenths / 10, s->totalMsTenths % 10);
   snprintf(values[4], STAT_VALUE_MAX, "%d.%d ms", s->networkMsTenths / 10, s->networkMsTenths % 10);
   snprintf(values[5], STAT_VALUE_MAX, "%d.%d ms", s->decodeMsTenths / 10, s->decodeMsTenths % 10);
   snprintf(values[6], STAT_VALUE_MAX, "%d.%d ms", s->presentMsTenths / 10, s->presentMsTenths % 10);
   snprintf(values[7], STAT_VALUE_MAX, "%d.%d ms", s->displayWaitMsTenths / 10, s->displayWaitMsTenths % 10);
   snprintf(values[8], STAT_VALUE_MAX, "%d", s->pipelineDepth);
   snprintf(values[9], STAT_VALUE_MAX, "%d", s->framesIncomplete);
   snprintf(values[10], STAT_VALUE_MAX, "%d", s->framesDroppedBehind);
}

// while streaming every button belongs to the PC - a game needs all of them. so the app keeps nothing
// for itself and uses SELECT as a modifier instead: the combos below. the buttons of a combo are held
// back from the PC, so the game never sees a stray press when one is used.
#define PAD_BIT(button) (1u << (button))

// the render loop publishes these for the pad thread; the thread never reads UI state directly
static volatile unsigned padForwardUiHeldBack = 0;   // buttons the mouse/keyboard UI owns, held back from the PC
static volatile int padForwardGamepad = 1;                 // 1 = drive the virtual gamepad, 0 = mouse and keyboard
static volatile int padForwardStop = 0;                    // tells the pad thread to exit
static sys_ppu_thread_t padForwardThreadId;

// forwards the live controller to the PC, minus the buttons we keep for ourselves. the SELECT-combo
// hold-back is worked out here from the live pad so a combo button never leaks a frame to the PC; the
// keyboard hold-back is published by the render loop (it changes rarely, so a little lag is fine).
static void sendPadStateToServer(void)
{
   unsigned down = getPadDownButtons();
   unsigned heldBack = padForwardUiHeldBack;

   // SELECT is the app's modifier, but only when a combo button is pressed with it - hold back SELECT and
   // the combo buttons then. SELECT on its own passes straight through as the gamepad's Select/Back.
   unsigned shortcutMask = getShortcutHeldBackMask();               // SELECT + every bound combo button
   unsigned comboButtons = shortcutMask & ~(1u << PAD_BTN_SELECT);
   if ((down & (1u << PAD_BTN_SELECT)) && (down & comboButtons)) heldBack |= shortcutMask;

   Stick left = getPadLeftStick(), right = getPadRightStick();
   sendPadState(down & ~heldBack, left.x, left.y, right.x, right.y);
}

// samples and sends the controller on its own steady clock, off the render loop (which stalls on the
// flip). this is what keeps PS3->PC input smooth and independent of video hitches, for every consumer.
static void padForwardThread(uint64_t arg)
{
   (void)arg;
   uint64_t lastModeSentUs = 0;
   int lastModeSent = -1;
   while (!padForwardStop) {
      pollPad();
      if (isStreamLive()) {
         // repeat the device mode about once a second, and right away when it changes: it is a lone UDP
         // packet, so a lost one would otherwise leave the PC driving the wrong device
         uint64_t nowUs = getTimeUs();
         if (padForwardGamepad != lastModeSent || nowUs - lastModeSentUs >= PAD_MODE_INTERVAL_US) {
            sendPadMode(padForwardGamepad);
            lastModeSent = padForwardGamepad;
            lastModeSentUs = nowUs;
         }
         sendPadStateToServer();
      }
      sys_timer_usleep(PAD_SEND_INTERVAL_US);
   }
   exitThread();
}

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   initRtc();
   initVfs();   // file backends for settings.txt; must precede any read/write
   logBuildVersion();
   appRegisterExitCallback();

   int netRc = initNet();
   logInfo("[cst] initNet rc=0x%x\n", netRc);
   registerWithBridge("app", "cell-stream");   // live logs in the bridge client's Logs tab

   // a decoded picture goes to the screen the moment it is ready, never waiting for the display's next
   // refresh - that wait costs ~8ms and buys us nothing here (measured; see the README)
   if (initGfx(GFX_VSYNC_OFF) != 0) return 1;
   if (initFont() != 0) return 1;
   initPad();
   loadShortcuts();             // SELECT+button combos from settings.txt (created with defaults if missing)
   int audioRc = initAudio();   // the mixer's PCM feed carries the PC's sound
   logInfo("[cst] initAudio rc=0x%x\n", audioRc);

   // restore the saved input and stream modes (loadShortcuts already created the file), and apply the stream
   // mode BEFORE connecting so the very first PLAY asks the server for the right size and rate
   InputMode inputMode = INPUT_CONTROLLER;   // game streaming is the main use; the mouse modes are a cycle away
   int streamMode = DEFAULT_STREAM_MODE;
   {
      char settingsText[SETTINGS_FILE_CAP];
      int settingsLength = readFile(CELL_STREAM_SETTINGS_PATH, settingsText, sizeof settingsText - 1);
      if (settingsLength > 0) {
         settingsText[settingsLength] = 0;
         inputMode = loadEnumSetting(settingsText, KEY_INPUT_MODE, inputModeTokens, INPUT_MODE_COUNT, INPUT_CONTROLLER);
         streamMode = loadStreamMode(settingsText);
      }
   }
   applyStreamMode(streamMode);

   initStream();   // finds the server and keeps a stream up by itself, for as long as the app runs

   Font font = openSystemFont(FONT_SANS);

   Label fieldLabels[STAT_LINES], valueLabels[STAT_LINES];
   char shownValues[STAT_LINES][STAT_VALUE_MAX];
   int i;
   for (i = 0; i < STAT_LINES; i++) {
      shownValues[i][0] = 0;
      initLabelRaw(&fieldLabels[i], &font, 0, 0, AUTO, AUTO, STAT_TEXT_SIZE, 0xFFB0B0B0, TEXT_NOWRAP, statFieldNames[i]);
      initLabelRaw(&valueLabels[i], &font, 0, 0, AUTO, AUTO, STAT_TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, "");
   }

   initToast(&font);
   initKeyboard(keyboardTheme);
   setKeyboardCloseButton(PAD_BTN_TRIANGLE);   // Triangle raises and lowers it; Square/Circle become space/backspace
   initCompanionPrompt(&font);    // the "server needed" + QR screen when nothing is streaming
   initShortcutHint(&font);       // the shortcut list shown briefly when a stream starts

   FrameTiming displayTiming;     // frame pacing measured at the flip; feeds the graphs
   resetFrameTiming(&displayTiming);

   int exitRequested = 0, statsVisible = 0;   // stats overlay off by default; the stats shortcut shows it
   int wasLive = 0, framesUntilBufferRelease = 0, hintPending = 0;

   spawnJoinableThread(&padForwardThreadId, padForwardThread, 0, THREAD_PRIORITY_HIGH, THREAD_STACK_SIZE_8KB, "cst-pad");

   while (!appExitRequested && !exitRequested) {
      appPoll();
      updatePadEdges();   // the pad thread owns the hardware read (pollPad); we only derive our UI edges
      updateToast();
      updateShortcutHint();

      int live = isStreamLive();

      // free an ended stream's video memory only after a few video-less frames have rendered, so the
      // RSX is guaranteed done with it (freeing straight away hard-froze the app). the next session
      // waits for this before allocating its own.
      if (wasLive && !live) framesUntilBufferRelease = 4;
      if (!live && framesUntilBufferRelease > 0 && --framesUntilBufferRelease == 0) releaseStreamBuffers();
      // a new session: reset the graph now, but hold the shortcut hint until the first frame actually
      // shows (the encoder's first frame can be seconds after the stream goes live)
      if (!wasLive && live) { hintPending = 1; resetFrameTiming(&displayTiming); }
      wasLive = live;

      if (live) {
         // every button belongs to the PC while streaming, so SELECT is the modifier for ours. the pad
         // thread holds back the combo buttons from the PC; here we only act on a fired combo.
         int selectHeld = isPadButtonDown(PAD_BTN_SELECT);
         if (selectHeld) {
            ShortcutAction action = firedShortcut();
            switch (action) {
            case SHORTCUT_INPUT_MODE:
               inputMode = (inputMode + 1) % INPUT_MODE_COUNT;
               if (inputMode != INPUT_MOUSE_KEYBOARD && isKeyboardOpen()) closeKeyboard();
               saveEnumSetting(KEY_INPUT_MODE, inputModeTokens[inputMode]);
               showToast(inputModeNames[inputMode]);
               break;
            case SHORTCUT_STREAMING_MODE:
               streamMode = (streamMode + 1) % STREAM_MODE_COUNT;
               applyStreamMode(streamMode);
               saveEnumSetting(KEY_STREAMING_MODE, streamModes[streamMode].token);
               showToast(streamModes[streamMode].name);
               break;
            case SHORTCUT_STATS:
               statsVisible = !statsVisible;
               showToast(statsVisible ? "Stats shown" : "Stats hidden");
               break;
            case SHORTCUT_CUSTOM1: case SHORTCUT_CUSTOM2:
            case SHORTCUT_CUSTOM3: case SHORTCUT_CUSTOM4: {
               int slot = action - SHORTCUT_CUSTOM1 + 1;
               sendCustomCommand(slot);
               char label[16];
               snprintf(label, sizeof label, "Custom %d", slot);
               showToast(label);
               break;
            }
            default: break;
            }
         }

         // mouse+keyboard mode: Triangle raises the on-screen keyboard and lowers it again (the keyboard's
         // own close button). while it is up it reads the d-pad/face buttons itself and they are held back
         // from the PC. a SELECT combo being entered takes priority, so the keyboard ignores input during it.
         if (inputMode == INPUT_MOUSE_KEYBOARD && !selectHeld) {
            if (isKeyboardOpen()) updateKeyboard();
            else if (isPadButtonPressed(PAD_BTN_TRIANGLE)) openKeyboard(onKeyboardKey);
         }
      } else {
         if (isPadButtonPressed(PAD_BTN_START)) exitRequested = 1;
      }

      // hand the pad thread what to forward; it samples and sends on its own steady clock
      // the keyboard owns its buttons while up; when it is down, mouse+keyboard mode still holds Triangle
      // back so the button that summons the keyboard never leaks a press to the PC
      if (isKeyboardOpen()) padForwardUiHeldBack = KEYBOARD_HELD_BACK;
      else if (inputMode == INPUT_MOUSE_KEYBOARD) padForwardUiHeldBack = PAD_BIT(PAD_BTN_TRIANGLE);
      else padForwardUiHeldBack = 0;
      padForwardGamepad = (inputMode == INPUT_CONTROLLER);

      // streaming, and no new picture: don't redraw the same one, just keep the loop cheap
      if (live && !isNewStreamPictureReady()) { sleepMs(1); continue; }
      if (!live) sleepMs(16);   // no video to pace us: don't spin the waiting screen flat out

      StreamStats streamStats;
      getStreamStats(&streamStats);

      if (live) {   // re-render only the stat values that changed
         char current[STAT_LINES][STAT_VALUE_MAX];
         formatStatValues(&streamStats, current);
         for (i = 0; i < STAT_LINES; i++) {
            if (strcmp(current[i], shownValues[i]) != 0) {
               strcpy(shownValues[i], current[i]);
               setLabelText(&valueLabels[i], current[i]);
            }
         }
      }

      beginGfxFrame();
      clearGfx(COLOR_BLACK);
      if (live) {
         drawStreamFrame();
         if (hintPending) { showShortcutHint(); hintPending = 0; }   // first frame shown: start the 10s hint
      } else {
         // no stream: tell the user the Windows companion server is needed, with a QR to it,
         // and keep the shortcut list up the whole time we wait
         drawCompanionPrompt();
         drawShortcutHint();
      }
      if (statsVisible && live) {
         // one bordered panel: the "Label: value" list, then the frame-time graph, then the Hide footer
         int graphHeight = getFrametimeGraphHeight(), hintHeight = getStatsHideHintHeight();
         int panelWidth = GRAPH_WIDTH + STATS_PADDING * 2;
         int panelHeight = STATS_PADDING * 2 + STAT_LINES * STAT_LINE_STEP + STATS_GRAPH_GAP + graphHeight
                         + STATS_HINT_GAP + hintHeight;
         fillGfxRectangle(STATS_X, STATS_Y, panelWidth, panelHeight, PANEL_BACKGROUND);
         strokeGfxRectangle(STATS_X, STATS_Y, panelWidth, panelHeight, 1, PANEL_BORDER);

         int contentX = STATS_X + STATS_PADDING, firstLineY = STATS_Y + STATS_PADDING;
         for (i = 0; i < STAT_LINES; i++) {
            int lineY = firstLineY + i * STAT_LINE_STEP;
            fieldLabels[i].x = contentX;
            fieldLabels[i].y = lineY;
            drawLabel(&fieldLabels[i]);
            valueLabels[i].x = contentX + STAT_VALUE_X;
            valueLabels[i].y = lineY;
            drawLabel(&valueLabels[i]);
         }
         int graphY = firstLineY + STAT_LINES * STAT_LINE_STEP + STATS_GRAPH_GAP;
         drawFrametimeGraph(&displayTiming, contentX, graphY, GRAPH_WIDTH);
         drawStatsHideHint(STATS_X * 2 + panelWidth, graphY + graphHeight + STATS_HINT_GAP);   // centred under the panel
      }
      if (live) {
         drawKeyboard();   // docks bottom-right; no-op unless open
         if (isShortcutHintActive()) drawShortcutHint();   // lingers a few seconds after the stream starts
      }
      drawToast();
      endGfxFrame();
      if (live) {
         noteStreamFlipWait(getGfxFlipWaitUs());     // what waiting for the display cost us
         noteFrame(&displayTiming, getTimeUs());     // frame pacing, for the stats graph
      }
   }

   padForwardStop = 1;                      // stop forwarding before the stream socket goes away
   joinThread(padForwardThreadId);

   stopStream();
   while (isStreamRunning()) sleepMs(10);   // let the connect loop shut the decoder down cleanly
   for (i = 0; i < 4; i++) {   // flush video-less frames through the RSX before freeing its buffers
      beginGfxFrame();
      clearGfx(COLOR_BLACK);
      endGfxFrame();
   }
   releaseStreamBuffers();

   termKeyboard();
   freeToast();
   freeShortcutHint();
   freeCompanionPrompt();
   for (i = 0; i < STAT_LINES; i++) { freeLabel(&fieldLabels[i]); freeLabel(&valueLabels[i]); }
   closeFont(&font);
   termFont();
   termAudio();
   termGfx();
   shutdownVfs();
   return 0;
}
