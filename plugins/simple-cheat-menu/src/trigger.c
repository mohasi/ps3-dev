// Cheat-menu pad trigger + capture state machine.
//
// The PS button is the only trigger, split by hold length. A SHORT press is left
// alone for the classify beat then opens the cheat menu and captures input (vsh
// libpad blinded, raw kernel reads take over); circle drops back to the in-game
// XMB, PS resumes the game. A LONG press (PS still held when the overlay opens) is
// the quit/power menu — passed through untouched. We tell them apart by reading the
// raw pad the instant a PS overlay opens: PS still held = long press. The export
// detour on the paf per-frame callback shows/hides the overlay (overlay.cpp) as
// menuOpen flips. No impose hooking: during raw gameplay the game owns the pad and
// cellPadGetData is silent; while a PS overlay is up vsh owns the pad and reports
// flow — that activity is the overlay-open signal (hardware 2026-07-09).

#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <cell/pad.h>

#include "dbg.h"
#include "printf.h"   // snprintf: format the dump-count status message
#include "vsh.h"
#include "thread.h"
#include "vsh-ext.h"
#include "export-hook.h"
#include "overlay.h"
#include "stats-overlay.h" // in-game stats counter, drawn from the same frame hook
#include "cheat-sync.h"    // loadSyncMode + syncMode: create/read settings at startup
#include "cheat-fetch.h"   // maybeFetchForGame: proactive online cheat download at game launch
#include "raw-pad.h"
#include "nav-repeat.h"
#include "trigger.h"
#include "texture-patch.h"   // dumpTextures + patch apply/revert for the Patches tab

// rsx overclocking from the Stats tab, shimmed in overlay-bridge.c
void stepStatsCoreClock(int direction);
void stepStatsMemoryClock(int direction);

#define TAG "[cht] "

// paf per-frame callback ("Framework_Begin"). the proven-hookable frame
// entry the FPS counter rides; we detour it to drive the overlay from the
// frame thread.
#define PAF_FRAMEWORK_BEGIN_FNID  0x59BDA198u

#define PAD_POLL_USEC      (33 * 1000)   // ~30 Hz
#define SENSOR_POLL_USEC   (500 * 1000)  // stats counter clocks + temps, twice a second

// ps-menu detection thresholds, in poll ticks
#define PS_MENU_QUIET_TICKS  30   // ~1s with no pad report = ps menu closed
#define PS_MENU_ARM_TICKS    150  // ignore the first ~5s after game launch (transient reports)

// capture release thresholds, in poll ticks
#define CLASSIFY_TIMEOUT_TICKS 30    // ~1s to see the first raw report and classify short vs long
#define CAPTURE_QUIET_TICKS    60    // ~2s with no raw report = pad gone / menu closed under us (safety net)
#define FETCH_SETTLE_TICKS     180   // ~6s in-game before the online cheat fetch, so the toast lands on the
                                     // actual game, not the boot splash/black screen (inGame flips true the
                                     // instant the process spawns, long before the game draws its first frame)
#define VOTE_DISMISS_TICKS     45    // ~1.5s to hold the vote outcome message before the rows return

// PS button as seen in the raw HID report while captured (hardware-observed
// 2026-07-09; distinct from every face/dpad/select/start value). libpad is
// blinded during capture so impose never sees PS — we detect it and resume
// the game ourselves via endInGameXmb().
#define CAPTURE_PS_MASK  0x100

// volatile: written by menuThread, read by the paf frame hook on vsh's thread.
static volatile int menuOpen = 0;

// current cheat-row selection. written by menuThread (d-pad edges), read by the
// paf frame hook which moves the highlight bar on vsh's frame thread.
static volatile int menuSelection = 0;

// runs on vsh's frame thread every frame. keep it light: show or hide the box
// to match menuOpen. widget create/update must happen on this thread (it is the
// one paf composites on), which is why drawing rides the frame hook.
static void onPafFrame(void)
{
   updateStatsOverlay();   // always on, menu or not — and it must run before the freeze check
   if (overlayFrameFrozen()) return;   // parked while a live Update re-parses; also does the post-parse repaint
   if (menuOpen) {
      overlayShowBox();
      overlayFlushContent();          // switch rows <-> centered message when update mode changes
      if (!overlayIsUpdating()) {     // during an update the rows/highlight are hidden
         overlayHighlightRow(menuSelection);
         overlayFlushTogglePaint();   // repaint any toggle the menu thread just flipped
      }
   } else {
      overlayHideBox();
   }
}

// worker thread: services the cheat job queue (scan/poke) off the menu and frame
// threads so neither ever blocks. it only touches state while the menu is open
// (overlayServiceJobs is a no-op otherwise), so it costs nothing during play.
static void jobWorkerThread(uint64_t arg)
{
   (void)arg;
   uint64_t sinceSensorPollUsec = 0;
   for (;;) {
      int did = overlayServiceJobs();
      uint32_t sleepUsec = did ? 2000 : 50000;   // busy: 2ms between rows; idle: 50ms
      sys_timer_usleep(sleepUsec);

      // the stats counter's clocks and temperatures, read here rather than on the frame thread,
      // and not read at all unless the counter is actually up: these are syscalls
      sinceSensorPollUsec += sleepUsec;
      if (sinceSensorPollUsec >= SENSOR_POLL_USEC) {
         sinceSensorPollUsec = 0;
         if (isStatsCounterActive()) pollStatsSensors();
      }
   }
}

// background pad read via cellPadGetData — webMAN's vsh_menu uses exactly
// this for open-detection (main.c:1529). the raw-HID syscall path is NOT
// safe to poll continuously: hammering sys_hid_manager_read through the
// cached io_pad_object across the game-launch transition (pad ownership
// moving to the game, handles going stale) hard-locks lv2. raw-HID is for
// M3 only, once the menu is open with the game paused. buttons packed
// low=word1 (L3/R3/START/dpad/SELECT) high=word2 (L1/R1/L2/R2/face).
// returns 0 on a real report, -1 if no port reported this poll.
static int readPad(uint32_t *buttonsOut)
{
   CellPadData data;

   *buttonsOut = 0;
   for (int32_t port = 0; port < 8; port++) {
      if (cellPadGetData(port, &data) == CELL_PAD_OK && data.len > 0) {
         *buttonsOut = (uint32_t)data.button[CELL_PAD_BTN_OFFSET_DIGITAL1]
                     | ((uint32_t)data.button[CELL_PAD_BTN_OFFSET_DIGITAL2] << 8);
         return 0;
      }
   }
   return -1;
}

// gather the running title/version + the cheat's identity and send a vote PR (non-blocking; a worker
// does the github calls). used by Mark Working / Mark Failed. WORKED carries the live working-val
// snapshot; FAILED sends an empty body. returns 1 if a vote actually started (so the caller can show
// the sending message), 0 if it was declined.
static int sendCheatVote(int row, VoteEvent event)
{
   char titleId[16];
   overlayGetTitleId(titleId, sizeof titleId);
   char version[16];
   getAppVersion(titleId, version, sizeof version);
   char body[512];
   if (event == VOTE_WORKED) overlayBuildVoteBody(row, body, sizeof body);
   else                      body[0] = '\0';
   return submitCheatVote(titleId, version, overlayGetCheatHash(row), event, body);
}

static void menuThread(uint64_t arg)
{
   (void)arg;
   logInfo(TAG "thread start\n");

   // section: startup — settings, wait for xmb + paf tree, install the frame hook, resolve the raw pad
   // create the data dir + settings.txt right away (file I/O works before XMB is up),
   // so the file exists to edit and the mode is known before any game launches - no
   // waiting on XMB-ready or the first menu open.
   loadSyncMode();
   loadStatsSettings();   // which parts of the stats counter are switched on
   logInfo(TAG "sync mode=%d\n", syncMode);

   // wait for XMB, ~60s budget
   int ticks = 0;
   while (!isXmbReady()) {
      sys_timer_sleep(1);
      if (++ticks > 60) { logError(TAG "xmb ready timeout\n"); exitThread(); return; }
   }
   logInfo(TAG "xmb ready\n");

   // the paf widget tree isn't up at the instant xmb reports ready, so poll
   // for page_notification for a few seconds (the fps counter waits the same
   // way) before we can parent the overlay onto it.
   unsigned int pageNotification = 0;
   for (int attempt = 0; attempt < 20; attempt++) {
      pageNotification = overlayFindPageNotification();
      if (pageNotification) break;
      sys_timer_sleep(1);
   }
   logInfo(TAG "overlay: page_notification=0x%x\n", pageNotification);

   // detour the paf frame callback — proven working.
   int rc = installExportHook("paf", PAF_FRAMEWORK_BEGIN_FNID, onPafFrame);
   logInfo(TAG "frame-hook install rc=%d\n", rc);

   // resolve the raw pad object now, on the stable xmb; reads come later and
   // only while the ps menu is up. failure just disables the capture path.
   int rawPadUsable = initRawPad() == 0;

   // ps-overlay detection + classify + capture. detection: in-game, cellPad
   // reports flowing = a PS overlay opened (~1s silence = closed); the arm
   // delay skips the transient reports around game launch. on open we capture
   // and read raw to classify: PS held = long press -> show the cheat menu and
   // keep capturing (circle drops back, PS resumes the game via endInGameXmb);
   // PS released = short press -> hand the pad back, leave the in-game XMB
   // alone. the classify and raw-quiet counters are the safety nets (the menu
   // stays open until circle/PS, the game exits, or the pad goes silent).
   uint32_t quietTicks = PS_MENU_QUIET_TICKS;   // start "quiet" so boot doesn't show
   uint32_t inGameTicks = 0;
   int      lastInGame = -1;
   int      fetchAttempted = 0;   // one online cheat-fetch per game session (reset on exit)
   int      captured = 0, dismissed = 0, psReleased = 0, classified = 0;
   int      blockedLogged = 0;
   uint32_t captureTicks = 0, rawQuietTicks = 0;
   int      selection = 0;        // current cheat row, moved by the d-pad
   int      voteDismissTicks = 0; // frames left showing a vote outcome before the rows return
   char     dumpStatus[32];       // holds the "Dumped N textures" message (overlaySetUpdating keeps the pointer)
   uint32_t prevDpad = 0;         // last raw buttons, for the edge-triggered cross toggle
   NavRepeat navUp = { 0, 0, 0 }, navDown = { 0, 0, 0 };   // held-direction auto-repeat
   int      statsTitlePending = 0;   // a game launched; waiting for its id to say whether it is one
   for (;;) {
      // section: game-boundary bookkeeping
      int inGame = getGameProcessId() != 0;
      inGameTicks = inGame ? inGameTicks + 1 : 0;

      if (inGame != lastInGame) {
         logInfo(TAG "inGame=%d\n", inGame);
         lastInGame = inGame;

         // the stats counter goes down either way: on a launch it stays down until the title id
         // proves this is a real game rather than homebrew, which is the same test the menu uses.
         notifyStatsGameChanged(0);
         statsTitlePending = inGame;
         if (!inGame) {
            // game gone: forget applied state (its match addresses died with the process) and
            // arm the no-touch hide, THEN publish menuOpen=0 so the hide frame sees the arm and
            // drops our widgets (vsh is freeing them) instead of re-colouring freed memory.
            overlayOnGameExit();
            __sync_synchronize();
            menuOpen = 0;   // the arena is kept and reused across games, never freed
            fetchAttempted = 0;   // next game gets its own fetch
         }
      }

      // section: is the running title a game? Homebrew and tools run as a process too, so the
      // counter waits for a readable title id before deciding whether to come up. The id is not
      // there the instant the process spawns, hence the poll.
      if (statsTitlePending) {
         char statsTitleId[16];
         overlayGetTitleId(statsTitleId, sizeof statsTitleId);
         if (statsTitleId[0]) {
            statsTitlePending = 0;
            notifyStatsGameChanged(isGameTitleId(statsTitleId));
         }
      }

      // section: proactive online fetch — once the game has settled and its title id is readable,
      // download its cheats in the background so they are local by the first PS press.
      // guarded to once per game; maybeFetchForGame itself skips offline/non-game/already-local.
      if (inGame && !fetchAttempted && inGameTicks > FETCH_SETTLE_TICKS) {
         char titleId[16];
         overlayGetTitleId(titleId, sizeof titleId);
         if (titleId[0]) { fetchAttempted = 1; maybeFetchForGame(titleId); }
      }

      // section: resolve a finished Update download. if the menu is still up, resolve it in place (refresh
      // on success, just leave update mode otherwise). if the user backed out, the menu is gone — fall
      // back to a toast, and (on success) reload the new file on the next open.
      UpdateResult updateResult = consumeUpdateResult();
      if (updateResult != UPDATE_NONE) {
         if (menuOpen) {
            if (updateResult == UPDATE_SAVED) {
               overlayRefreshFromFile();
               int rowCount = overlayGetRowCount();
               if (selection >= rowCount) selection = rowCount > 0 ? rowCount - 1 : 0;
               menuSelection = selection;
            } else {
               overlaySetUpdating(0);   // not-found / error: leave update mode, rows come back unchanged
            }
         } else {
            overlaySetUpdating(0);
            if (updateResult == UPDATE_SAVED)          { overlayScheduleReload(); vshNotify("Cheats updated  "); }
            else if (updateResult == UPDATE_NOT_FOUND) vshNotify("No cheats found for this game!  ");
            else                                       vshNotify("Couldn't update cheats  ");
         }
      }

      // section: resolve a finished vote — swap the "Marking..." message to the outcome for a moment,
      // then let the rows return. votes originate from a menu button, so the menu is normally still open.
      int voteResult = consumeVoteResult();
      if (voteResult) {
         if (menuOpen) { overlaySetUpdating(voteResult == 1 ? "Vote submitted. Thank you" : "Couldn't send vote"); voteDismissTicks = VOTE_DISMISS_TICKS; }
         else overlaySetUpdating(0);
      }
      if (voteDismissTicks > 0 && --voteDismissTicks == 0) overlaySetUpdating(0);

      // section: captured — raw reads drive classify + the menu input
      if (captured) {
         // captured: raw reads are the only input source. every path out of
         // here MUST restore the vsh pad (see raw-pad.h).
         uint32_t rawButtons = 0;
         int rawLength = readRawPad(&rawButtons);
         if (rawLength > 0) rawQuietTicks = 0;
         else if (rawQuietTicks < CAPTURE_QUIET_TICKS) rawQuietTicks++;
         captureTicks++;
         int psHeld = (rawButtons & CAPTURE_PS_MASK) != 0;

         if (!classified) {
            // is the opening PS still held? released = short press = our
            // trigger, show the cheat menu. held = long press = the quit/power
            // menu, leave it completely alone. decide on the first real report
            // (first read may be len 0).
            if (rawLength > 0) {
               classified = 1;
               if (psHeld) {
                  setVshPadEnabled(1);   // long press: hand the pad straight back, untouched
                  captured = 0;
                  dismissed = 1;
               } else if (overlayPrepareForTitle() > 0) {
                  // short press AND this title has cheats: show the menu. the file
                  // read+parse happened just now on this thread, so the frame
                  // thread's show is instant (no stall).
                  __sync_synchronize();   // release: publish the parsed cheatCount + pools before menuOpen
                  menuOpen = 1;
                  psReleased = 1;    // PS already up; a fresh PS press can exit
                  selection = 0;     // start on the first row
                  prevDpad = 0;
                  menuSelection = 0;
                  navUp.wasDown = navDown.wasDown = 0;   // fresh press edge on the first held direction
               } else {
                  // no cheats for this title: don't show the box at all, hand the pad straight back so
                  // the normal in-game XMB behaves as usual. no log - the toast tells the user, and a
                  // PS press per minute of gameplay would fill the file
                  setVshPadEnabled(1);   // restore the pad BEFORE the toast (not during the blind)
                  captured = 0;
                  dismissed = 1;
                  const char *toast = overlayNoCheatsMessage();
                  if (toast) vshNotify(toast);
               }
            } else if (captureTicks > CLASSIFY_TIMEOUT_TICKS) {
               setVshPadEnabled(1);   // no report to classify on; fail safe to passthrough
               captured = 0;
               dismissed = 1;
               logInfo(TAG "classify timeout - passthrough\n");
            }
         } else {
            // the cheat menu is up and captured. only treat PS as "exit to
            // game" once the opening hold has lifted.
            if (!psHeld && rawLength >= 0) psReleased = 1;

            // circle is edge-triggered here — it both backs out of a patch's options and drops to the
            // XMB. computed before prevDpad is updated below so the edge is real; backOut stops the same
            // press from doing both (back out AND drop to XMB) on one tick.
            uint32_t circleMask = (uint32_t)(CELL_PAD_CTRL_CIRCLE << 8);
            int circleEdge = (rawButtons & circleMask) && !(prevDpad & circleMask);
            int backOut = 0;

            // menu input, all disabled while an update is in flight (only XMB/PS work then):
            // d-pad moves the selection (auto-repeat via navFire), cross toggles the row, triangle
            // starts an update. these only set flags — the worker does the scan/poke.
            if (rawLength >= 0 && !overlayIsUpdating()) {
               int rowCount = overlayGetRowCount();

               // Stats Counter tab, SELECT held: the d-pad tunes the RSX clocks instead of moving
               // the selection, the same combination thermal-bench uses. Up/Down is the core clock,
               // Left/Right the memory clock. Held down, the memory clock still only moves one step
               // per settling period, which is enforced where the poke happens.
               int tuningClocks = overlayGetTab() == OVERLAY_TAB_STATS && (rawButtons & CELL_PAD_CTRL_SELECT);
               if (tuningClocks) {
                  if (navFire(&navUp,   (rawButtons & CELL_PAD_CTRL_UP)    != 0)) stepStatsCoreClock(+1);
                  if (navFire(&navDown, (rawButtons & CELL_PAD_CTRL_DOWN)  != 0)) stepStatsCoreClock(-1);
                  if ((rawButtons & CELL_PAD_CTRL_RIGHT) && !(prevDpad & CELL_PAD_CTRL_RIGHT)) stepStatsMemoryClock(+1);
                  if ((rawButtons & CELL_PAD_CTRL_LEFT)  && !(prevDpad & CELL_PAD_CTRL_LEFT))  stepStatsMemoryClock(-1);
               } else {
                  if (navFire(&navUp,   (rawButtons & CELL_PAD_CTRL_UP)   != 0) && selection > 0)            selection--;
                  if (navFire(&navDown, (rawButtons & CELL_PAD_CTRL_DOWN) != 0) && selection < rowCount - 1) selection++;
               }

               int currentTab = overlayGetTab();
               int patchesTab = currentTab == OVERLAY_TAB_PATCHES;
               int statsTab   = currentTab == OVERLAY_TAB_STATS;
               int inOptions  = overlayInPatchOptions();

               // L1/R1 cycle the tabs (Cheats, Patches, Stats Counter), wrapping at either end.
               // reset the selection to the top of the new list.
               uint32_t l1Mask = (uint32_t)(CELL_PAD_CTRL_L1 << 8);
               uint32_t r1Mask = (uint32_t)(CELL_PAD_CTRL_R1 << 8);
               if ((rawButtons & r1Mask) && !(prevDpad & r1Mask)) {
                  overlaySwitchTab((currentTab + 1) % OVERLAY_TAB_COUNT); selection = 0; menuSelection = 0;
               }
               if ((rawButtons & l1Mask) && !(prevDpad & l1Mask)) {
                  overlaySwitchTab((currentTab + OVERLAY_TAB_COUNT - 1) % OVERLAY_TAB_COUNT); selection = 0; menuSelection = 0;
               }

               // cross: Cheats tab toggles the row; Patches tab toggles a part (in options), drills into a
               // patch that has options, or turns a plain patch on/off.
               uint32_t crossMask = (uint32_t)(CELL_PAD_CTRL_CROSS << 8);
               if ((rawButtons & crossMask) && !(prevDpad & crossMask) && rowCount > 0) {
                  if (statsTab) { toggleStatsRow(selection); overlayRefreshRows(); }
                  else if (!patchesTab) overlayRequestToggle(selection);
                  else if (inOptions) {
                     overlaySetUpdating("Applying...");   // shown only during the (short) texture rebuild
                     overlayToggleSelectedPart(selection);
                     overlaySetUpdating(0);              // rows + the updated toggle come straight back
                  } else if (overlayPatchHasParts(selection)) {
                     overlayEnterPatchOptions(selection); selection = 0; menuSelection = 0;
                  } else {
                     overlaySetUpdating("Applying...");   // frame thread paints this while the toggle below blocks us
                     int result = overlayToggleSelectedPatch(selection);
                     overlaySetUpdating(result == 1 ? "Patch applied" : result == 2 ? "Patch removed" : "No matching textures found");
                     voteDismissTicks = VOTE_DISMISS_TICKS;   // hold the result briefly, then the rows return
                  }
               }

               // circle inside a patch's options backs out to the patch list (never drops to XMB — see backOut).
               if (inOptions && circleEdge) { overlayExitPatchOptions(); selection = 0; menuSelection = 0; backOut = 1; }

               // square (Patches tab, patch list): dump the on-screen textures (heap structs + the command
               // buffer's binds) to dumps/<titleId>/ for Patch Studio. reads only while the game is frozen
               // under the overlay — reading the live command buffer during play hard-locks lv2.
               uint32_t squareMask = (uint32_t)(CELL_PAD_CTRL_SQUARE << 8);
               if ((rawButtons & squareMask) && !(prevDpad & squareMask) && patchesTab && !inOptions) {
                  char dumpTitleId[16];
                  overlayGetTitleId(dumpTitleId, sizeof dumpTitleId);
                  overlaySetUpdating("Dumping textures...");   // frame thread paints this while the dump below blocks us
                  int added = dumpTextures(getGameProcessId(), dumpTitleId);
                  if (added < 0)       overlaySetUpdating("Dump failed");
                  else if (added == 0) overlaySetUpdating("No new textures");
                  else { snprintf(dumpStatus, sizeof dumpStatus, "Dumped %d textures", added); overlaySetUpdating(dumpStatus); }
                  voteDismissTicks = VOTE_DISMISS_TICKS;
               }

               // triangle: on the Patches tab, drill into the selected patch's options (if it has any); on the
               // Cheats tab (online only), re-download this title's cheats in the background — the cheats stay
               // ON during the download and the menu refreshes live on success, a centered "Updating…" meanwhile.
               uint32_t triangleMask = (uint32_t)(CELL_PAD_CTRL_TRIANGLE << 8);
               int triangleEdge = (rawButtons & triangleMask) && !(prevDpad & triangleMask);
               if (triangleEdge && patchesTab && !inOptions && overlayPatchHasParts(selection)) {
                  overlayEnterPatchOptions(selection); selection = 0; menuSelection = 0;
               } else if (triangleEdge && !patchesTab && syncMode != SYNC_OFFLINE) {
                  char titleId[16];
                  overlayGetTitleId(titleId, sizeof titleId);
                  // enter update mode ONLY if the fetch actually started; otherwise the menu would be
                  // stuck in "Updating..." with no fetch to ever clear it (e.g. a non-game title id).
                  if (updateCheatsForGame(titleId)) overlaySetUpdating("Updating cheats...");   // ASCII dots; hides the rows
               }

               // start (contribute only): mark the selected cheat WORKED — send an anonymous vote PR
               // carrying the live pre-write snapshot (working-val). only for an APPLIED cheat, so the
               // vote has real evidence. non-blocking (a worker runs the github calls).
               uint32_t startMask = (uint32_t)CELL_PAD_CTRL_START;
               if ((rawButtons & startMask) && !(prevDpad & startMask) && !patchesTab && syncMode == SYNC_CONTRIBUTE && overlayIsCheatApplied(selection)) {
                  int r = sendCheatVote(selection, VOTE_WORKED);
                  if (r == 1)      overlaySetUpdating("Marking as working...");   // centered message; input locks until the vote returns
                  else if (r == 2) { overlaySetUpdating("Already marked"); voteDismissTicks = VOTE_DISMISS_TICKS; }
               }

               // select (contribute only): mark the selected cheat FAILED — anonymous vote PR, empty
               // body (a failed original isn't stored). allowed whether or not the cheat is applied.
               uint32_t selectMask = (uint32_t)CELL_PAD_CTRL_SELECT;
               if ((rawButtons & selectMask) && !(prevDpad & selectMask) && !patchesTab && !statsTab && syncMode == SYNC_CONTRIBUTE && rowCount > 0) {
                  int r = sendCheatVote(selection, VOTE_FAILED);
                  if (r == 1)      overlaySetUpdating("Marking as failed...");
                  else if (r == 2) { overlaySetUpdating("Already marked"); voteDismissTicks = VOTE_DISMISS_TICKS; }
               }

               menuSelection = selection;
            }
            if (rawLength >= 0) prevDpad = rawButtons;   // track edges even while updating (so a held button doesn't re-fire on exit)

            // PS (released then pressed again) resumes the game; circle drops
            // back to the ps menu. the rest are safety releases.
            int psExit = psHeld && psReleased;
            const char *releaseReason = 0;
            if (psExit)                                        releaseReason = "ps";
            else if (circleEdge && !backOut)                   releaseReason = "circle";
            else if (!inGame)                                  releaseReason = "game exited";
            else if (rawLength < 0)                            releaseReason = "read error";
            else if (rawQuietTicks >= CAPTURE_QUIET_TICKS)     releaseReason = "raw quiet";

            if (releaseReason) {
               // restore libpad FIRST, then resume the game. endInGameXmb ONLY
               // resumes when libpad is already restored — called while blinded
               // it does nothing, the impose stays up, and the menu loops. (also
               // stop raw polling before the ownership change: captured=0 here,
               // and no raw read happens past this point.)
               overlayCancelAllPending();   // drop any in-flight scans; keep applied cheats
               setVshPadEnabled(1);
               captured = 0;
               menuOpen = 0;      // frame hook hides the overlay
               dismissed = 1;     // don't re-capture until the impose closes (pad quiet)
               blockedLogged = 0;
               quietTicks = 0;
               logInfo(TAG "capture: releasing (%s)\n", releaseReason);
               if (psExit) { logInfo(TAG "capture: endInGameXmb\n"); endInGameXmb(); }   // log before the risky resume call
            }
         }
      } else {
         // section: not captured — watch for a PS overlay opening, then begin capture
         uint32_t fresh = 0;
         if (readPad(&fresh) == 0) quietTicks = 0;
         else if (quietTicks < PS_MENU_QUIET_TICKS) quietTicks++;

         int psMenuOpen = inGameTicks > PS_MENU_ARM_TICKS && quietTicks < PS_MENU_QUIET_TICKS;

         if (!psMenuOpen && dismissed) {
            dismissed = 0;     // ps menu closed after a circle-dismiss; re-arm
            blockedLogged = 0;
         }

         // pad active but still dismissed = a press we're NOT acting on. log it
         // once per stuck period so a silent miss (no rising edge) is visible.
         if (psMenuOpen && dismissed && rawPadUsable && !blockedLogged) {
            logInfo(TAG "ps activity BLOCKED (dismissed still set, quietTicks=%u)\n", quietTicks);
            blockedLogged = 1;
         }

         if (psMenuOpen && !dismissed && rawPadUsable) {
            // a PS overlay opened. capture and read raw to classify it; the
            // box is shown only if it turns out to be a long press. blinding
            // libpad briefly on a short press is harmless (restored next tick).
            setVshPadEnabled(0);
            captured = 1;
            classified = 0;
            captureTicks = 0;
            rawQuietTicks = 0;
            psReleased = 0;    // wait for the opening PS to lift before it can exit
         }
      }

      sys_timer_usleep(PAD_POLL_USEC);
   }
}

void startCheatMenuThreads(void)
{
   sys_ppu_thread_t tid;
   spawnThread(&tid, menuThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "cheat-menu-main");
   spawnThread(&tid, jobWorkerThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "cheat-menu-worker");
}
