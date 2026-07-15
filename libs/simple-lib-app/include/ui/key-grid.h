#pragma once

// key-grid - shared engine behind keyboard.h and hex-pad.h: a docked, controller-navigable
// grid of single-character keys. D-pad moves the highlighted cell; Cross commits it; Square
// is backspace; Circle closes; any extraBindings fire a fixed character regardless of cursor
// position (e.g. the keyboard's Triangle-is-space). Docks itself at the screen's bottom-right.
//
// call updateKeyGrid()/drawKeyGrid() once per frame regardless of open state - both no-op
// while closed, same convention as stats.h.

#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "button-repeat.h"

#define KEY_GRID_MAX_ROWS 5
#define KEY_GRID_MAX_COLS 14

// flat/metro palette for the grid, supplied by the app (the lib stays theme-agnostic): the docked
// panel is a filled box with a border, the selected cell a filled box in the highlight colours.
typedef struct {
   uint32_t panelFill, panelBorder;
   uint32_t keyHighlightFill, keyHighlightBorder;
   uint32_t keyText;
   int      borderThickness;
} KeyGridTheme;

typedef void (*KeyGridCallback)(char key);

// returns the label text to render for a grid cell's character (e.g. a multi-char "TAB").
typedef const char *(*KeyGridLabelTextFn)(char key);

// returns the font size to render a grid cell's character at (e.g. smaller for a multi-char
// label). NULL means every cell uses the picker's base font size.
typedef int (*KeyGridFontSizeFn)(char key);

// a button that always commits a fixed character, independent of cursor position (e.g. the
// keyboard's Triangle-is-space) - checked in addition to the built-in Cross/Square/Circle.
typedef struct {
   PadButton button;
   char      key;
} KeyGridExtraBinding;

typedef struct {
   int rows, cols;
   int cellW, cellH;
   int baseFontSize;
   const char *grid;   // rows*cols chars, row-major

   const KeyGridExtraBinding *extraBindings;
   int extraBindingCount;

   Font         font;
   KeyGridTheme theme;
   Label        keyLabels[KEY_GRID_MAX_ROWS][KEY_GRID_MAX_COLS];

   int cursorRow, cursorCol;
   int isOpen;
   int armed;          // 0 on the frame it opens, so the opening Cross press isn't also read as a key commit
   int panelX, panelY;   // top-left, set fresh each time the grid opens
   int panelW, panelH;

   KeyGridCallback onKey;

   ButtonRepeat moveUpRepeat, moveDownRepeat, moveLeftRepeat, moveRightRepeat;
} KeyGridPicker;

// grid/labelText/fontSizeFor/extraBindings are all borrowed references - the caller must keep
// them alive for the picker's lifetime (they're static data in every current caller).
void initKeyGrid(KeyGridPicker *kg, int rows, int cols, int cellW, int cellH, int baseFontSize,
                  const char *grid, KeyGridLabelTextFn labelText, KeyGridFontSizeFn fontSizeFor,
                  const KeyGridExtraBinding *extraBindings, int extraBindingCount, KeyGridTheme theme);
void rethemeKeyGrid(KeyGridPicker *kg, KeyGridTheme theme);   // recolour for a live theme switch
void termKeyGrid(KeyGridPicker *kg);

void openKeyGrid(KeyGridPicker *kg, KeyGridCallback onKey);
void closeKeyGrid(KeyGridPicker *kg);
int  isKeyGridOpen(KeyGridPicker *kg);

// true while the grid is open AND L2 is held - the caller's document (text editor, hex viewer,
// ...) should read the d-pad itself during this window instead of leaving it to the grid, so
// its caret/cursor can be repositioned without closing the grid. The grid itself hides while
// this is true (drawKeyGrid() no-ops) and ignores its own input; releasing L2 hands focus - and
// the d-pad - back to the grid.
int isKeyGridBackgroundFocused(KeyGridPicker *kg);

void updateKeyGrid(KeyGridPicker *kg);
void drawKeyGrid(KeyGridPicker *kg);
