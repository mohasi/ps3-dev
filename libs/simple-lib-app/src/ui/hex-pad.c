// hex-pad - see ui/hex-pad.h. Thin wrapper over the shared grid engine in ui/key-grid.h.
#include "ui/hex-pad.h"
#include "ui/key-grid.h"

#define PAD_ROWS   4   // 0-9/A-F
#define PAD_COLS   4
#define KEY_CELL_W 50
#define KEY_CELL_H 50
#define FONT_SIZE  26

// hex digits 0-F, laid out 4x4. backspace has no grid cell - it's bound straight to Square by
// the shared key-grid engine (same convention as keyboard.c).
static const char PAD_GRID[PAD_ROWS][PAD_COLS] = {
   { '0', '1', '2', '3' },
   { '4', '5', '6', '7' },
   { '8', '9', 'A', 'B' },
   { 'C', 'D', 'E', 'F' },
};

static const char *getPadKeyLabelText(char key)
{
   static char text[2];
   text[0] = key;
   text[1] = '\0';
   return text;
}

static KeyGridPicker padGrid;

void initHexPad(KeyGridTheme theme)
{
   initKeyGrid(&padGrid, PAD_ROWS, PAD_COLS, KEY_CELL_W, KEY_CELL_H,
               FONT_SIZE, &PAD_GRID[0][0], getPadKeyLabelText, NULL, NULL, 0, theme);
}

void rethemeHexPad(KeyGridTheme theme) { rethemeKeyGrid(&padGrid, theme); }
void termHexPad(void) { termKeyGrid(&padGrid); }

void openHexPad(HexPadKeyCallback onKey) { openKeyGrid(&padGrid, onKey); }
void closeHexPad(void) { closeKeyGrid(&padGrid); }
int  isHexPadOpen(void) { return isKeyGridOpen(&padGrid); }
int  isHexPadBackgroundFocused(void) { return isKeyGridBackgroundFocused(&padGrid); }

void updateHexPad(void) { updateKeyGrid(&padGrid); }
void drawHexPad(void) { drawKeyGrid(&padGrid); }
