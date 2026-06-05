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

// number of items the current selection acts on: the checked rows, or 1 for the
// active row when nothing is checked. 0 in an empty directory.
int getSelectionCount(void);

// name of the highlighted row, regardless of any checkbox marks; NULL in an empty
// directory. used by rename, which targets the active row only. the pointer is
// valid until the selection or directory changes.
const char *getActiveEntryName(void);

// renames the highlighted row to newName within the current directory (ignoring
// checkboxes), then refreshes the listing with the cursor on the renamed item.
// rejects an invalid name or one already in use (no overwrite); same name is a
// no-op. returns 0 on success, -1 otherwise.
int renameActiveEntry(const char *newName);

// deletes the current selection (all checked rows, or the active row when none
// are checked), then refreshes the listing. the cursor lands on the row above
// the topmost deleted item, or the row below it when that was the top of the list.
// also clears the cut clipboard.
void deleteSelection(void);

// puts the current selection (checked rows, or the active row when none are
// checked) onto the clipboard for a move (cut) or a duplicate (copy), replacing
// any previous clipboard contents.
void cutSelection(void);
void copySelection(void);

// applies the clipboard into the current directory via the progress overlay
// (a background worker), then refreshes the listing. see runPaste (paste.h)
// for the move/copy/overwrite/no-op semantics.
void pasteClipboard(void);
