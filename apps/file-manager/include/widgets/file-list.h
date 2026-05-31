#pragma once

// file-list - scrollable directory listing widget

#include "font.h"
#include "gfx.h"
#include "audio.h"
#include "ui/breadcrumb.h"
#include "selection-actions.h"

#define FILE_LIST_PAGE_SIZE  9

void initFileList(Font *font, GfxTexture spritesheet, Audio *clickSfx, Audio *checkSfx, int x, int y, int maxWidth, int rowHeight, int fontSize, uint32_t color, Breadcrumb *bc);
void termFileList(void);
void updateFileList(void);
void drawFileList(void);

// queried by the home screen when the action menu is opened.
// pointers are valid until the selection or directory changes.
// folder sizes are walked progressively in the background under a per-folder
// time budget; until a folder is sized the size column shows an em dash, and
// once sized a trailing '+' marks a result that hit the budget (approximate).
// the sidepanel just takes a snapshot of whatever is known at open time.
const SelectionSummary *getSelectionSummary(void);
const SelectionAction  *getAvailableActions(int *outCount);
