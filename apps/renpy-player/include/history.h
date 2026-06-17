#pragma once

#include "assets.h"   // SPR_MAX (sprite-snapshot size)

#define HISTORY_OVERLAY_MAX 8   // == OV_ACTIVE_MAX (the active HUD overlay set, snapshotted per frame)

// The rollback timeline: one Frame per Say line, so the player can step back/forward through the
// dialogue. Strings are stable bytecode-table pointers; shows[]/showAt[] snapshot the sprites that
// were on screen. This module is a dumb store + cursor -- the screen records and restores frames.
typedef struct {
   const char *who, *what, *scene;
   const char *shows[SPR_MAX];
   const char *showAt[SPR_MAX];
   int         showAtl[SPR_MAX];   // each sprite's atl id, so rollback restores ATL placement/anim
   int         showCount;
   int         nvl;            // was this an NVL line?
   int         ingame;         // was this an in-game chat-box line? (Say kind=2)
   int         isPause;        // a renpy.pause() checkpoint (no dialogue) -- restore visuals, no textbox
   int         nvlPage;        // which NVL page it belonged to (bumped on enter / nvl clear)
   const char *musicCmd;       // music playing on this line (NULL = none)
   const void *overlays[HISTORY_OVERLAY_MAX];   // the active HUD overlay SET at this line (RbcOverlay*)
   int         overlayCount;   // so rollback restores which config.overlay_functions were active
   void       *varSnap;        // snapshot of the variable store at this line (owned by history.c)
   int         varSnapOwned;   // 1 = this frame allocated varSnap; 0 = shares an earlier frame's
} Frame;

void resetHistory(void);   // clear the timeline

Frame       *appendHistory(void);    // a zeroed slot for a new line (cursor -> it), or NULL if full
const Frame *getHistoryCurrent(void);   // the frame the cursor is on (NULL if empty)
const Frame *getHistoryLatest(void);    // the most recent frame (NULL if empty)

int isHistoryAtLatest(void);     // is the cursor on the newest line?
int stepHistoryBack(void);     // move the cursor back one (1 = moved)
int stepHistoryForward(void);  // move the cursor forward one (1 = moved, never past the latest)

// Rebuild the NVL page as it stood at the cursor: the run of frames sharing the cursor's page id,
// capped at `max` (older lines having scrolled off). Fills who[]/what[] and returns the count.
int getHistoryNvlPage(const char **who, const char **what, int max);
