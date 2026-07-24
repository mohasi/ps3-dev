// key-grid - see ui/key-grid.h
#include "ui/key-grid.h"

#define PANEL_PAD     16
#define SCREEN_MARGIN 16   // clear of the screen's right/bottom edges

void initKeyGrid(KeyGridPicker *kg, int rows, int cols, int cellW, int cellH, int baseFontSize,
                  const char *grid, KeyGridLabelTextFn labelText, KeyGridFontSizeFn fontSizeFor,
                  const KeyGridExtraBinding *extraBindings, int extraBindingCount, KeyGridTheme theme)
{
   kg->rows             = rows;
   kg->cols             = cols;
   kg->cellW            = cellW;
   kg->cellH            = cellH;
   kg->baseFontSize     = baseFontSize;
   kg->grid             = grid;
   kg->extraBindings    = extraBindings;
   kg->extraBindingCount = extraBindingCount;
   kg->closeButton      = PAD_BTN_CIRCLE;   // Circle closes by default; callers can move or disable it
   kg->theme            = theme;
   kg->panelW = PANEL_PAD * 2 + cols * cellW;
   kg->panelH = PANEL_PAD * 2 + rows * cellH;

   kg->font = openSystemFont(FONT_POP);

   for (int row = 0; row < rows; row++) {
      for (int col = 0; col < cols; col++) {
         char key  = grid[row * cols + col];
         int  size = fontSizeFor ? fontSizeFor(key) : baseFontSize;
         initLabelRaw(&kg->keyLabels[row][col], &kg->font, 0, 0, AUTO, AUTO, size, theme.keyText, TEXT_NOWRAP, labelText(key));
      }
   }
}

void rethemeKeyGrid(KeyGridPicker *kg, KeyGridTheme theme)
{
   kg->theme = theme;
   for (int row = 0; row < kg->rows; row++)
      for (int col = 0; col < kg->cols; col++)
         setLabelColor(&kg->keyLabels[row][col], theme.keyText);
}

void termKeyGrid(KeyGridPicker *kg)
{
   for (int row = 0; row < kg->rows; row++)
      for (int col = 0; col < kg->cols; col++)
         freeLabel(&kg->keyLabels[row][col]);
   closeFont(&kg->font);
}

void openKeyGrid(KeyGridPicker *kg, KeyGridCallback onKey)
{
   kg->onKey     = onKey;
   kg->cursorRow = 0;
   kg->cursorCol = 0;
   kg->panelX = getGfxScreenWidth()  - kg->panelW - SCREEN_MARGIN;
   kg->panelY = getGfxScreenHeight() - kg->panelH - SCREEN_MARGIN;
   kg->isOpen = 1;
   kg->armed  = 0;
}

void closeKeyGrid(KeyGridPicker *kg) { kg->isOpen = 0; }
int  isKeyGridOpen(KeyGridPicker *kg) { return kg->isOpen; }
void setKeyGridCloseButton(KeyGridPicker *kg, int button) { kg->closeButton = button; }

int isKeyGridBackgroundFocused(KeyGridPicker *kg)
{
   if (!kg->isOpen) return 0;
   return isPadButtonDown(PAD_BTN_L2);
}

void updateKeyGrid(KeyGridPicker *kg)
{
   if (!kg->isOpen) return;
   if (!kg->armed) { kg->armed = 1; return; }   // swallow the press that opened the grid

   if (kg->closeButton >= 0 && isPadButtonPressed((PadButton)kg->closeButton)) { closeKeyGrid(kg); return; }
   if (isKeyGridBackgroundFocused(kg)) return;   // L2 held: d-pad and commit buttons belong to the document underneath

   if (isRepeatDue(&kg->moveDownRepeat, getPadButtonState(PAD_BTN_DOWN)))
      kg->cursorRow = (kg->cursorRow + 1) % kg->rows;
   else if (isRepeatDue(&kg->moveUpRepeat, getPadButtonState(PAD_BTN_UP)))
      kg->cursorRow = (kg->cursorRow - 1 + kg->rows) % kg->rows;
   else if (isRepeatDue(&kg->moveRightRepeat, getPadButtonState(PAD_BTN_RIGHT)))
      kg->cursorCol = (kg->cursorCol + 1) % kg->cols;
   else if (isRepeatDue(&kg->moveLeftRepeat, getPadButtonState(PAD_BTN_LEFT)))
      kg->cursorCol = (kg->cursorCol - 1 + kg->cols) % kg->cols;

   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      if (kg->onKey) kg->onKey(kg->grid[kg->cursorRow * kg->cols + kg->cursorCol]);
      return;
   }
   // Square backspaces; Circle backspaces too, unless it is being used to close the grid
   if (isPadButtonPressed(PAD_BTN_SQUARE) || (kg->closeButton != PAD_BTN_CIRCLE && isPadButtonPressed(PAD_BTN_CIRCLE))) {
      if (kg->onKey) kg->onKey('\b');
      return;
   }
   for (int i = 0; i < kg->extraBindingCount; i++) {
      if (isPadButtonPressed(kg->extraBindings[i].button)) {
         if (kg->onKey) kg->onKey(kg->extraBindings[i].key);
         return;
      }
   }
}

void drawKeyGrid(KeyGridPicker *kg)
{
   if (!kg->isOpen || isKeyGridBackgroundFocused(kg)) return;

   drawGfxBox(kg->panelX, kg->panelY, kg->panelW, kg->panelH, kg->theme.borderThickness, kg->theme.panelFill, kg->theme.panelBorder);

   for (int row = 0; row < kg->rows; row++) {
      int cellY = kg->panelY + PANEL_PAD + row * kg->cellH;
      for (int col = 0; col < kg->cols; col++) {
         int cellX = kg->panelX + PANEL_PAD + col * kg->cellW;
         if (row == kg->cursorRow && col == kg->cursorCol)
            drawGfxBox(cellX, cellY, kg->cellW, kg->cellH, kg->theme.borderThickness, kg->theme.keyHighlightFill, kg->theme.keyHighlightBorder);

         Label *l = &kg->keyLabels[row][col];
         drawLabelAt(l, cellX + (kg->cellW - l->tt.tex.w) / 2, cellY + (kg->cellH - l->tt.tex.h) / 2);
      }
   }
}
