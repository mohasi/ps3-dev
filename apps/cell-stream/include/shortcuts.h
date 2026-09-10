#pragma once

// shortcuts - user-editable SELECT+button combos, one per action, read from
// /dev_hdd0/tmp/cell-stream/settings.txt (created with defaults on first launch).
// SELECT is always the modifier; the file only names which button, held with
// SELECT, fires each action. See docs/settings-and-shortcuts.md.

#include "pad.h"

typedef enum {
   SHORTCUT_INPUT_MODE,       // cycle: mouse+keyboard -> controller
   SHORTCUT_STREAMING_MODE,   // cycle: vsync off -> vsync -> vsync + one-frame buffer
   SHORTCUT_STATS,            // toggle the stats overlay
   SHORTCUT_CUSTOM1,          // PC command, defined in the server's Custom Commands tab
   SHORTCUT_CUSTOM2,
   SHORTCUT_CUSTOM3,
   SHORTCUT_CUSTOM4,
   SHORTCUT_COUNT
} ShortcutAction;

// reads settings.txt into the combo table, writing the file with defaults if missing
void loadShortcuts(void);

// while SELECT is held: the action whose button was just pressed this frame, or
// SHORTCUT_COUNT when none. only one action fires per frame.
ShortcutAction firedShortcut(void);

// every bound button (SELECT included), so the caller can hold them back from the
// PC - the streamed game must never see a stray press from one of our combos.
unsigned getShortcutHeldBackMask(void);

// the action's display name, for the on-screen shortcut hint.
const char *getShortcutActionName(ShortcutAction action);

// the button an action is bound to (for mapping to a controller glyph); PAD_BTN_SELECT when unbound
PadButton getShortcutButton(ShortcutAction action);
