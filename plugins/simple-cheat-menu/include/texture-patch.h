#pragma once

#include <stdint.h>

// texture dump-and-replace for a running game, driven from the cheat menu's Patches tab. all reads and
// writes go through ps3mapi (game-mem.h) while the game is paused under the in-game xmb. results and
// errors go to the dbg log with the [tex] tag.

#ifdef __cplusplus
extern "C" {
#endif

// accumulating dump: read the on-screen textures' pixels from video memory and write any not
// already present to /dev_hdd0/tmp/simple-cheat-menu/dumps/<titleId>/<contentHash>.bin, appending
// to that title's manifest.txt. per-title so different games don't collide, and it survives game
// restarts (never auto-wiped). returns how many NEW textures were written this scan.
int dumpTextures(uint32_t pid, const char *titleId);

// list the patch folder names under /dev_hdd0/tmp/simple-cheat-menu/patches/<titleId> into `names`
// (a flat buffer of maxNames * nameCap bytes; folder i lands at names + i*nameCap, null-terminated).
// returns how many were listed (0 if the title has no patches folder). the Patches menu tab shows these.
int listPatchNames(const char *titleId, char *names, int nameCap, int maxNames);

// apply the patch at patches/<titleId>/<patchName>/ to the running game: each manifest entry replaces a
// live texture matched by its original content hash with the replacement file, after snapshotting the
// original bytes so the patch can be turned off again. marks the patch active. returns textures replaced.
int applyPatch(uint32_t pid, const char *titleId, const char *patchName);

// turn a patch off: restore each replaced texture from its snapshot. returns the number restored.
int revertPatch(uint32_t pid, const char *titleId, const char *patchName);

// one selectable part of a patch, for the drill-in menu. group < 0 means the part stands alone; parts
// sharing a group index belong together, and pickOne means that group is a radio (variants) rather than
// free toggles. name is the author's label ("Blue Dog").
typedef struct { char name[40]; int group; int pickOne; } PatchPart;

// list a patch's parts (0 for a whole, partless patch that applies via applyPatch). fills up to maxParts.
int getPatchParts(const char *titleId, const char *patchName, PatchPart *out, int maxParts);

// set exactly which parts are on: partOn holds one flag per part. rebuilds every texture the patch
// touches to last-wins, snapshotting originals and restoring them where no part wins. returns textures
// left holding a replacement. this is the parts equivalent of applyPatch/revertPatch.
int rebuildPatch(uint32_t pid, const char *titleId, const char *patchName, const unsigned char *partOn);

// game exited: wipe the title's original-texture snapshots (they died with the process).
void clearAppliedState(const char *titleId);

#ifdef __cplusplus
}
#endif
