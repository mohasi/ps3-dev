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

void initConfirmOverlay(Audio *clickSfx);

// shows a modal prompt and reports which button was pressed. it carries up to
// three buttons - cross, an optional middle square, and circle - each with its
// own label; passing NULL for a label hides that button. pass squareText = NULL
// for a plain two-button prompt; pass a label to add the third option, e.g.
// rename's Merge / Replace / Cancel. a two-button caller just checks for
// CONFIRM_CROSS. crossText = NULL with only a circle label makes an information
// card the user can read and close, with no action to take.
void askConfirm(const char *title, const char *message,
                const char *crossText, const char *squareText, const char *circleText,
                ConfirmCallback onResult);

void rethemeConfirmOverlay(void);   // recolour the dialog text for a live theme switch

extern Overlay confirmOverlay;
