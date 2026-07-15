#pragma once

// sidepanel - slides in from the right; shows a header for the current selection
// and a vertical list of available actions. emits chosen action via handler.

#include "overlay.h"
#include "gfx.h"
#include "audio.h"
#include "selection-actions.h"

typedef void (*SelectionActionHandler)(SelectionAction action);

void initSidepanel(GfxTexture spritesheet, Audio *clickSfx, SelectionActionHandler handler);
void setSidepanelContent(const SelectionSummary *summary, const SelectionAction  *actions, int count);
void rethemeSidepanel(void);   // recolour the header + row text for a live theme switch

extern Overlay sidepanel;
