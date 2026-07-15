// keyboard - see ui/keyboard.h. Thin wrapper: the grid nav/L2-handoff/panel engine lives in
// ui/key-grid.h, shared with hex-pad.c.
#include "ui/keyboard.h"
#include "ui/key-grid.h"

#define KEYBOARD_ROWS 5
#define KEYBOARD_COLS 14   // 12 character columns + two extra columns for symbol pairs/Tab/Return
#define KEY_CELL_W    50
#define KEY_CELL_H    50

#define FONT_SIZE       26
#define SMALL_FONT_SIZE 16   // "TAB"/"RET" need to fit inside one ordinary key cell

// one fixed layout: digits/dashes, then the qwerty rows, then a symbol row, with
// the two extra columns holding the symbol pairs most common in dev text side by
// side ({} <> |~ ?`), plus Tab and Return stacked at the very bottom-right, where
// a physical keyboard has them. no shift/caps.
static const char KEY_GRID[KEYBOARD_ROWS][KEYBOARD_COLS] = {
   { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '_', '{', '}' },
   { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '<', '>' },
   { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '\\', '|', '~' },
   { 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', ':', '"', '?', '\t' },
   { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '+', '=', '`', '\n' },
};

static const char *getKeyLabelText(char key)
{
   if (key == '\t') return "TAB";
   if (key == '\n') return "RET";
   static char text[2];
   text[0] = key;
   text[1] = '\0';
   return text;
}

static int getKeyFontSize(char key) { return (key == '\t' || key == '\n') ? SMALL_FONT_SIZE : FONT_SIZE; }

// Triangle always types a space, independent of cursor position - a physical keyboard's spacebar.
static const KeyGridExtraBinding EXTRA_BINDINGS[] = { { PAD_BTN_TRIANGLE, ' ' } };

static KeyGridPicker keyGrid;

void initKeyboard(KeyGridTheme theme)
{
   initKeyGrid(&keyGrid, KEYBOARD_ROWS, KEYBOARD_COLS, KEY_CELL_W, KEY_CELL_H,
               FONT_SIZE, &KEY_GRID[0][0], getKeyLabelText, getKeyFontSize, EXTRA_BINDINGS, 1, theme);
}

void rethemeKeyboard(KeyGridTheme theme) { rethemeKeyGrid(&keyGrid, theme); }
void termKeyboard(void) { termKeyGrid(&keyGrid); }

void openKeyboard(KeyboardKeyCallback onKey) { openKeyGrid(&keyGrid, onKey); }
void closeKeyboard(void) { closeKeyGrid(&keyGrid); }
int  isKeyboardOpen(void) { return isKeyGridOpen(&keyGrid); }
int  isBackgroundFocused(void) { return isKeyGridBackgroundFocused(&keyGrid); }

void updateKeyboard(void) { updateKeyGrid(&keyGrid); }
void drawKeyboard(void) { drawKeyGrid(&keyGrid); }
