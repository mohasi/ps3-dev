#pragma once

// Always-on stats counter drawn in the screen corner, over the XMB and over a running
// game. Separate from the cheat menu panel: it stays up while the menu is closed, so it
// owns its own widgets, its own heap page and its own parent tracking.
//
// It draws only while a game is running, which is the only time its numbers mean anything, and
// only when switched on in the menu's Stats Counter tab. Anything switched off is not built at
// all rather than hidden, because the firmware walks every child of the page on every frame, so a
// hidden widget still costs.
//
// Unlike the cheat panel, this draws during raw gameplay, which means it is running while the
// firmware tears its widget tree down at every game start and end. Hardware showed exactly what
// that costs: a widget left attached through a teardown is harmless, but writing to one while
// the teardown is under way locks the console, and nothing we can poll says it has begun.
//
// So this follows the VshFpsCounter reference's design, which does this same job and survives:
// widget memory from the firmware's own allocator, the widget named so it can be found among its
// parent's children, and before every write it asks the parent whether it still owns the widget.
// The firmware unregisters children as it destroys a page, so that question is what reveals a
// teardown. On a no, the widget is forgotten untouched; while attached, it is destroyed properly.

#ifdef __cplusplus
extern "C" {
#endif

// The rows of the menu's Stats Counter tab, in the order they are listed. STATS_ROW_ENABLED is
// the master: with it off every other row is inert and the counter is not drawn at all.
// STATS_ROW_POSITION is not an on/off but a left/right choice, so it shows its side instead.
// The frame rate and the average frame time have no row: they are the point of the counter and
// are always shown when it is on.
enum StatsRow {
   STATS_ROW_ENABLED,
   STATS_ROW_GRAPH,
   STATS_ROW_CLOCKS,
   STATS_ROW_TEMPS,
   STATS_ROW_POSITION,
   STATS_ROW_COUNT
};

// MENU thread: what the tab lists. The label is the row's name; the value is what its right hand
// column shows ("ON", "OFF", "Top Left", "Top Right").
const char *getStatsRowLabel(int row);
const char *getStatsRowValue(int row);

// MENU thread: is this row currently inert because the master switch is off? Those rows are
// listed dimmed and do nothing when pressed.
int isStatsRowDisabled(int row);

// MENU thread: flip a row and save the settings. The counter rebuilds itself on the next frame,
// so what is on screen follows immediately.
void toggleStatsRow(int row);

// STARTUP: read the saved settings. Without this every row takes its built-in default.
void loadStatsSettings(void);

// FRAME thread, every frame from the paf hook: time the frame and repaint when due.
void updateStatsOverlay(void);

// WORKER thread, a couple of times a second: read the RSX clocks and the temperatures into
// memory for the drawing thread to display. These are syscalls and a hypervisor peek and have no
// business on the frame thread, which only ever reads the numbers left here.
void pollStatsSensors(void);

// PAD thread, when a game starts or ends: put the counter down and hold it down while vsh
// rebuilds the widget tree, then bring it back only if a game is running. Told rather than
// detected, because asking vsh for the running game on the drawing thread is what locked the
// console at game exit. Being a poll interval late costs nothing against a two second settle.
void notifyStatsGameChanged(int inGame);

#ifdef __cplusplus
}
#endif
