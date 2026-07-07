#pragma once

// hex-pad - compact on-screen keypad for entering hex byte values with a
// controller. 0-9/A-F laid out 4x4. D-pad moves the highlighted key; Cross
// commits it; Square is backspace; Circle closes the pad. Docks itself at the
// screen's bottom-right, same conventions as keyboard.h (including the
// L2-held "hand focus to the document underneath" behavior).

#include "gfx.h"

typedef void (*HexPadKeyCallback)(char key);   // '0'-'9', 'A'-'F', or '\b'

void initHexPad(GfxTexture sprites, SpriteRegion panelSprite, int panelCap);
void termHexPad(void);

void openHexPad(HexPadKeyCallback onKey);
void closeHexPad(void);
int  isHexPadOpen(void);

// see keyboard.h's isBackgroundFocused() - same L2-held handoff, scoped to the hex pad.
int isHexPadBackgroundFocused(void);

void updateHexPad(void);
void drawHexPad(void);
