#pragma once

// keyboard - compact on-screen keyboard for controller-only text entry.
// English, single layout (no shift/caps) - lowercase letters, digits, the
// punctuation dev work actually needs, plus a pair of extra columns holding
// {} <> |~ ?` and small Tab/Return keys stacked at the bottom-right. D-pad
// moves the highlighted key; Cross commits it; Triangle is space, Square is
// backspace; Circle closes the keyboard. Docks itself at the screen's
// bottom-right.
//
// call updateKeyboard()/drawKeyboard() once per frame regardless of open
// state - both no-op while closed, same convention as stats.h.

#include "gfx.h"

// backspace, space, tab, and return report as ordinary characters
// ('\b', ' ', '\t', '\n'), so callers can treat every key the same way -
// append/erase from a text buffer.
typedef void (*KeyboardKeyCallback)(char key);

void initKeyboard(GfxTexture sprites, SpriteRegion panelSprite, int panelCap);
void termKeyboard(void);

void openKeyboard(KeyboardKeyCallback onKey);
void closeKeyboard(void);
int  isKeyboardOpen(void);

// true while the keyboard is open AND L2 is held - the caller's document
// (text editor, hex viewer, ...) should read the d-pad itself during this
// window instead of leaving it to the keyboard, so the caret/cursor can be
// repositioned without closing the keyboard. The keyboard itself hides while
// this is true (drawKeyboard() no-ops) and ignores its own input; releasing
// L2 hands focus - and the d-pad - back to the keyboard.
int isBackgroundFocused(void);

void updateKeyboard(void);
void drawKeyboard(void);
