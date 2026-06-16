#pragma once

#include "assets.h"   // SPR_MAX
#include "vm.h"       // VM_CALL_STACK_MAX

// Generic VM-state save/load for the classic game menu. This is the ONE permitted deviation from the
// engine: instead of Ren'Py's Python pickle, a game's restorable state is serialised as a flat,
// engine-agnostic blob -- the bytecode execution point (pc + call stack), the whole variable store,
// and the on-screen scene (background + sprites + music) + the line being shown. Nothing here is
// specific to any one game; it captures exactly the state our VM owns, so it maps to any title.
//
// savestate pulls the VM + variable store itself (it owns the format). The screen passes the visual
// state it tracks (the current line + the scene snapshot from the rollback frame), and on load gets
// that visual state back to rebuild the scene and re-show the line.

// The visual + line half of a save (the VM + vars are applied internally on load).
typedef struct {
    char who[96];
    char what[600];
    int  nvl;
    int  ingame;   // in-game chat-box line (Say kind=2)
    char scene[160];
    char music[160];
    int  showCount;
    char shows[SPR_MAX][120];
    char showAt[SPR_MAX][48];
    int  showAtl[SPR_MAX];   // each sprite's atl id, so load restores ATL placement/anim (like rollback)
} SaveScene;

// Capture the current state to slot `display` ("1".."50", "a1".., "q1"..). The screen supplies the
// line + scene it is showing; the VM position and variables are read here. `saveName` is the slot's
// display label (store.save_name / chapter). Returns 0 on success.
int saveStateCapture(const char *display, const SaveScene *scene, const char *saveName);

// Load slot `display`: restores the variable store and VM position internally, and returns the scene
// + line in *out for the screen to rebuild. Returns 0 on success, <0 if missing/corrupt.
int saveStateLoad(const char *display, SaveScene *out);

// For the file picker: does the slot exist?
int saveSlotExists(const char *display);

// Fills outTime (the save file's mtime in config.time_format "%b %d, %H:%M") and outName (the saved
// label). Returns 1 if the slot exists (so the picker shows "n. <time>\n<name>"), 0 if empty.
int saveSlotInfo(const char *display, char *outTime, int timeCap, char *outName, int nameCap);

// Cheap stat-only modification time (unix seconds) -> *out. Returns 1 if the slot exists. Used to
// find the "newest" slot, which the engine renders with the selected_ role (greyed text).
int saveSlotMtime(const char *display, long *out);

// Pixel-exact screenshot saved alongside a slot as a PNG sidecar slot-<display>.png. write: `argb`
// is w*h*4 A8R8G8B8 (the captured + cropped thumbnail) -> encoded via the SDK codec (0 on success).
// saveThumbPath builds the file path so the picker can load it back with loadGfxTexture.
int  saveThumbWrite(const char *display, const unsigned char *argb, int w, int h);
void saveThumbPath(const char *display, char *out, int cap);
