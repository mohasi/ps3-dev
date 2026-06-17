// button - icon + label button with enabled/disabled rendering
#include "ui/button.h"

static const int BUTTON_ICON_LABEL_GAP = 14;
static const int BUTTON_DISABLED_ALPHA = 0x66;
// label sits this many px below the icon's vertical centre — the text texture
// includes ascender padding above the glyphs.
static const int BUTTON_LABEL_Y_OFFSET = 3;

void initButton(Button *button, Image icon, Label label, ButtonState state)
{
   button->icon  = icon;
   button->label = label;
   button->state = state;
}

void freeButton(Button *button)
{
   freeLabel(&button->label);
}

void setButtonState(Button *button, ButtonState state)
{
   button->state = state;
}

void moveButton(Button *button, int x, int y)
{
   int labelY = y + (button->icon.h - button->label.tt.tex.h) / 2 + BUTTON_LABEL_Y_OFFSET;
   moveImage(&button->icon, x, y);
   moveLabel(&button->label, x + button->icon.w + BUTTON_ICON_LABEL_GAP, labelY);
}

int getButtonWidth(Button *button)
{
   return button->icon.w + BUTTON_ICON_LABEL_GAP + button->label.tt.tex.w;
}

void drawButton(Button *button)
{
   int alpha = button->state == BUTTON_ENABLED ? 0xFF : BUTTON_DISABLED_ALPHA;
   drawImageAlpha(&button->icon, alpha);
   drawLabelAlpha(&button->label, alpha);
}
