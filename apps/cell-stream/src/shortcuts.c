// shortcuts - load the SELECT+button combo table from settings.txt (see shortcuts.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.

#include "shortcuts.h"
#include "cell-stream-settings.h"   // CELL_STREAM_SETTINGS_DIR / _PATH
#include "settings-file.h"      // loadSettingsFile / findSettingValue
#include "string-utilities.h"   // startsWith
#include "vfs.h"                // makeDirPath
#include "dbg.h"

#define SETTINGS_DIR  CELL_STREAM_SETTINGS_DIR
#define SETTINGS_PATH CELL_STREAM_SETTINGS_PATH

// the file written on first launch: every action at its default combo, documented inline
static const char *DEFAULT_SETTINGS =
   "# cell-stream shortcuts - edit over FTP; changes apply on the next launch.\n"
   "# hold SELECT plus the named button. custom1-4 are PC commands you define in\n"
   "# the server's Custom Commands tab; the PS3 only sends the slot name.\n"
   "\n"
   "input-mode     = select+cross      # cycle: mouse only -> mouse+keyboard -> controller\n"
   "streaming-mode = select+square     # cycle: 720p/60 vsync off -> vsync -> vsync + one-frame buffer\n"
   "stats          = select+r3         # show/hide the stats overlay\n"
   "custom1        = select+triangle   # default on PC: Game Bar (Guide button)\n"
   "custom2        = select+circle     # default on PC: Steam Big Picture\n"
   "custom3        = select+l1\n"
   "custom4        = select+r1\n"
   "\n"
   "# saved settings - updated as you change them in-stream, applied on the next launch\n"
   "saved-input-mode     = controller        # mouse | mouse+keyboard | controller\n"
   "saved-streaming-mode = 720p60-vsync       # 720p60-vsync-off | 720p60-vsync | 720p60-buffer\n";

// the settings.txt key each action reads, indexed by ShortcutAction
static const char *actionKeys[SHORTCUT_COUNT] = {
   "input-mode", "streaming-mode", "stats", "custom1", "custom2", "custom3", "custom4"
};

// button names as written after "select+" in a combo. SELECT is the modifier, never a trigger.
static const struct { const char *name; PadButton button; } buttonNames[] = {
   {"cross", PAD_BTN_CROSS}, {"circle", PAD_BTN_CIRCLE}, {"square", PAD_BTN_SQUARE},
   {"triangle", PAD_BTN_TRIANGLE}, {"l1", PAD_BTN_L1}, {"r1", PAD_BTN_R1},
   {"l2", PAD_BTN_L2}, {"r2", PAD_BTN_R2}, {"l3", PAD_BTN_L3}, {"r3", PAD_BTN_R3},
   {"start", PAD_BTN_START}, {"up", PAD_BTN_UP}, {"down", PAD_BTN_DOWN},
   {"left", PAD_BTN_LEFT}, {"right", PAD_BTN_RIGHT},
};

// PAD_BTN_SELECT stands for "unbound": SELECT is only ever the modifier, so it can
// never be a real trigger and makes a safe sentinel.
#define SHORTCUT_UNBOUND PAD_BTN_SELECT

static PadButton triggerButton[SHORTCUT_COUNT];

// does `text` begin with `name` followed by a token terminator (so "l1" doesn't match "l13")?
static int tokenMatches(const char *text, const char *name)
{
   int length = getStrLen(name);
   for (int i = 0; i < length; i++) if (text[i] != name[i]) return 0;
   char after = text[length];
   return after == 0 || after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '#';
}

// a combo value like "select+cross   # comment" -> the trigger button (SELECT implied)
static PadButton parseCombo(const char *value)
{
   if (!value) return SHORTCUT_UNBOUND;               // key missing/malformed - never crash on it
   while (*value == ' ' || *value == '\t') value++;
   if (startsWith(value, "select+")) value += 7;      // SELECT is implied; drop it

   for (unsigned i = 0; i < sizeof buttonNames / sizeof buttonNames[0]; i++)
      if (tokenMatches(value, buttonNames[i].name)) return buttonNames[i].button;
   return SHORTCUT_UNBOUND;
}

void loadShortcuts(void)
{
   makeDirPath(SETTINGS_DIR);

   char text[2048];
   if (loadSettingsFile(SETTINGS_PATH, DEFAULT_SETTINGS, text, sizeof text) == 1)
      logInfo("[cst] created %s with defaults\n", SETTINGS_PATH);

   // a key missing from the user's file falls back to its line in DEFAULT_SETTINGS
   for (int action = 0; action < SHORTCUT_COUNT; action++) {
      const char *value = findSettingValue(text, actionKeys[action]);
      if (!value) value = findSettingValue(DEFAULT_SETTINGS, actionKeys[action]);
      triggerButton[action] = parseCombo(value);
   }
}

ShortcutAction firedShortcut(void)
{
   if (!isPadButtonDown(PAD_BTN_SELECT)) return SHORTCUT_COUNT;
   for (int action = 0; action < SHORTCUT_COUNT; action++)
      if (triggerButton[action] != SHORTCUT_UNBOUND && isPadButtonPressed(triggerButton[action]))
         return (ShortcutAction)action;
   return SHORTCUT_COUNT;
}

unsigned getShortcutHeldBackMask(void)
{
   unsigned mask = 1u << PAD_BTN_SELECT;
   for (int action = 0; action < SHORTCUT_COUNT; action++)
      if (triggerButton[action] != SHORTCUT_UNBOUND) mask |= 1u << triggerButton[action];
   return mask;
}

static const char *actionNames[SHORTCUT_COUNT] = {
   "Input mode", "Streaming mode", "Stats", "Custom 1", "Custom 2", "Custom 3", "Custom 4"
};

// display name per PadButton, indexed by the enum in pad.h
static const char *buttonDisplayNames[] = {
   "D-Up", "D-Down", "D-Left", "D-Right", "Cross", "Circle", "Square", "Triangle",
   "L1", "R1", "L2", "R2", "Start", "Select", "L3", "R3"
};

const char *getShortcutActionName(ShortcutAction action)
{
   return action >= 0 && action < SHORTCUT_COUNT ? actionNames[action] : "";
}

const char *getShortcutButtonName(ShortcutAction action)
{
   if (action < 0 || action >= SHORTCUT_COUNT) return "-";
   PadButton button = triggerButton[action];
   return button == SHORTCUT_UNBOUND ? "-" : buttonDisplayNames[button];
}

PadButton getShortcutButton(ShortcutAction action)
{
   return action >= 0 && action < SHORTCUT_COUNT ? triggerButton[action] : PAD_BTN_SELECT;
}
