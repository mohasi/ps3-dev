#pragma once

// PAF overlay (implemented in overlay.cpp). Draws over a running game through
// vsh's PAF compositor — never the raw framebuffer, which hard-locks in-game.
//
// A PAF widget's second field is a C++ std::string (its name). PAF walks that
// string every frame, so the widget must be a *properly constructed* C++
// object — a zeroed buffer leaves the string's pointer null and faults the
// walk (that was the lockup). Hence this is C++, not C. We never set the widget
// NAME (so that string stays empty and never allocates); text IS set, copied by
// vsh's own allocator. The widgets live in an on-demand lv2 heap arena.

// the two menu tabs (L1/R1 switch). the Cheats tab lists cheats; the Patches tab lists the title's
// texture patches and applies one. shared with trigger.c so its input handler can route per tab.
#define OVERLAY_TAB_CHEATS   0
#define OVERLAY_TAB_PATCHES  1
#define OVERLAY_TAB_STATS    2
#define OVERLAY_TAB_COUNT    3

#ifdef __cplusplus
extern "C" {
#endif

// resolve the page_notification widget address (0 until system_plugin exists,
// so poll it during early boot). the widget we parent the overlay on.
unsigned int overlayFindPageNotification(void);

// MENU thread: read the running title's id and parse its cheat file (the file I/O that would
// otherwise stall the frame thread). returns non-zero if the menu should open: always for a
// real game, empty list or not, and for anything else only when it has cheats or patches.
// re-parses only when the title changed, so a live cheat's resolved address survives a re-open.
int overlayPrepareForTitle(void);

// MENU thread: the toast to show when the running title has no local cheats (NULL = none).
// the caller (which has vshNotify) shows it after restoring the pad.
const char *overlayNoCheatsMessage(void);

// MENU thread: copy the running game's title id into out (empty if none / not ready).
void overlayGetTitleId(char *out, int cap);

// show/hide a solid test box parented to page_notification. call from the paf
// frame hook (the thread paf widgets must be touched on). show creates it once
// then keeps it opaque; hide makes it transparent.
void overlayShowBox(void);
void overlayHideBox(void);

// MENU thread, on game exit: forget the applied-cheat state (it belongs to the dead
// process; its match addresses are stale). the next game starts with every row OFF.
void overlayOnGameExit(void);

// move the selection highlight to a row. call from the paf frame hook with the
// menu thread's current selection; ignored when the menu is hidden, there are
// no rows, or the row is unchanged. index is clamped to the rendered rows.
void overlayHighlightRow(int index);

// number of rendered cheat rows for the running title, so the menu thread can
// clamp its selection.
int overlayGetRowCount(void);

// MENU thread: a cheat's op-hash (its online identity), for the vote path. 0 if out of range.
unsigned int overlayGetCheatHash(int cheat);

// MENU thread: is this cheat live in the game right now? Mark Working is gated on this (a WORKED vote
// needs real evidence + the pre-write snapshot).
int overlayIsCheatApplied(int cheat);

// MENU thread: build the CHEAT_WORKED working-val body for a cheat (one "<opIdx>=<orig>" line per w32
// op, from the live snapshot). empty unless applied. returns the length written.
int overlayBuildVoteBody(int cheat, char *out, int cap);

// MENU thread: flip a row's desired on/off. never blocks (the worker does the
// scan/poke); toggling a pending row cancels it. call on the cross-button edge.
void overlayRequestToggle(int index);

// MENU thread: the active tab (OVERLAY_TAB_*), so the input handler routes Cross/Square per tab.
int overlayGetTab(void);

// MENU thread: repaint the visible rows on the next frame, after something changed what they say.
void overlayRefreshRows(void);

// MENU thread: switch tab (L1/R1). entering Patches re-lists the title's patch folders first. the
// caller resets its selection to 0 after. returns the new tab's row count.
int overlaySwitchTab(int tab);

// MENU thread: toggle the selected patch on/off (Cross on the Patches tab) — apply if off, revert if on.
// blocks briefly while it scans the game's textures. returns 1 = applied, 2 = reverted, 0 = nothing matched.
int overlayToggleSelectedPatch(int row);

// MENU thread: does the selected patch expose parts? if so Cross/Triangle drill into its options list
// instead of applying the whole patch.
int overlayPatchHasParts(int row);

// MENU thread: drill into the selected patch's parts (Triangle, or Cross on a patch with parts). the
// Patches tab then lists the parts until overlayExitPatchOptions. returns the part count, 0 if it has none
// (caller applies the whole patch instead). the caller resets its selection to 0 after.
int overlayEnterPatchOptions(int row);

// MENU thread: leave the parts list, back to the patch list (Circle inside the options).
void overlayExitPatchOptions(void);

// MENU thread: are we inside a patch's parts list right now, so the input handler routes Cross/Circle there.
int overlayInPatchOptions(void);

// MENU thread: toggle the selected part on/off (Cross inside the options). a pick-one variant turns its
// group siblings off first (radio), then the whole patch is rebuilt to last-wins. returns 1 = turned on,
// 2 = turned off, 0 = no game.
int overlayToggleSelectedPart(int row);

// MENU thread: enter update mode with a centered message, or leave it with msg == NULL. while
// updating, the rows + highlight are hidden and the message (a held string literal) is shown in their
// place; all menu input except XMB/PS is a no-op. our dimmer hides the XMB toasts, so progress shows
// inside the panel instead.
int  overlayIsUpdating(void);
void overlaySetUpdating(const char *msg);

// MENU thread: whether the menu is on screen right now (choose in-menu message vs a toast).
int  overlayIsMenuVisible(void);

// MENU thread: an Update finished while the menu was closed — reload the new file on the next open
// (revert the still-applied old cheats first, safely, then re-parse).
void overlayScheduleReload(void);

// FRAME thread, each frame while the menu is open: apply a pending content-mode switch (rows <-> message).
void overlayFlushContent(void);

// MENU thread, after a successful Update download (menu still open): revert the applied cheats,
// re-read the updated file, and re-enable the ones still present (matched by name) — all in place.
void overlayRefreshFromFile(void);

// FRAME thread, top of the paf hook: returns 1 while a live refresh is re-parsing (caller skips
// the frame). also performs the single post-parse repaint. no-op the rest of the time.
int overlayFrameFrozen(void);

// MENU thread, on menu exit: cancel every in-flight (pending) job, reverting it to
// whatever is already applied. settled ON cheats are left running.
void overlayCancelAllPending(void);

// WORKER thread: reconcile one row toward its desired state (scan/poke or revert).
// call in a loop; no-op while the menu is closed. returns 1 if it did work.
int overlayServiceJobs(void);

// FRAME thread: repaint the ON/OFF widgets from the computed display state. call
// every frame from the paf hook; only does work after a state changed.
void overlayFlushTogglePaint(void);

#ifdef __cplusplus
}
#endif
