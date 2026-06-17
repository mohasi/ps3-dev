#pragma once

// confirm-overlay - centered modal yes/no dialog drawn over a dimmed screen.
// generic: the caller supplies the title, message and both button labels, plus
// a callback that receives the choice. cross = yes (confirmed), circle = no.

#include "overlay.h"
#include "gfx.h"
#include "audio.h"
#include <stdbool.h>

// which button the user pressed.
typedef enum {
   CONFIRM_CROSS,
   CONFIRM_SQUARE,
   CONFIRM_CIRCLE
} ConfirmChoice;

typedef void (*ConfirmCallback)(ConfirmChoice choice);

void initConfirmOverlay(GfxTexture spritesheet, Audio *clickSfx);

// shows a modal prompt and reports which button was pressed. it carries up to
// three buttons - cross, an optional middle square, and circle - each with its
// own label. pass squareText = NULL for a plain two-button prompt (the middle
// button is hidden); pass a label to add the third option, e.g. rename's
// Merge / Replace / Cancel. a two-button caller just checks for CONFIRM_CROSS.
void askConfirm(const char *title, const char *message,
                const char *crossText, const char *squareText, const char *circleText,
                ConfirmCallback onResult);

extern Overlay confirmOverlay;
