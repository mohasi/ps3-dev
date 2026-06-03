#pragma once

// confirm-overlay - centered modal yes/no dialog drawn over a dimmed screen.
// generic: the caller supplies the title, message and both button labels, plus
// a callback that receives the choice. cross = yes (confirmed), circle = no.

#include "overlay.h"
#include "gfx.h"
#include "audio.h"
#include <stdbool.h>

typedef void (*ConfirmCallback)(bool confirmed);

// cross = yes (confirmed), circle = no.
void initConfirmOverlay(GfxTexture spritesheet, Audio *clickSfx);
void askConfirm(const char *title, const char *message, const char *yesText, const char *noText, ConfirmCallback onResult);

extern Overlay confirmOverlay;
