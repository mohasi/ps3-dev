#pragma once

// file-list - scrollable directory listing widget

#include "font.h"
#include "gfx.h"
#include "audio.h"
#include "ui/breadcrumb.h"
#include "selection-actions.h"

void initFileList(Font *font, GfxTexture spritesheet, Audio *clickSfx, Audio *checkSfx, int x, int y, int maxWidth, int rowHeight, int fontSize, uint32_t color, Breadcrumb *bc);
void termFileList(void);
void updateFileList(void);
void drawFileList(void);

// the directory currently being browsed (valid until the next directory change).
const char *getCurrentPath(void);

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

// renames the highlighted row (ignoring checkboxes) to newName in the current
// directory. validates the name and no-ops on an unchanged name; a free name
// renames at once, while a collision opens a Merge / Replace / Cancel prompt
// (Merge folds two folders together, Replace clobbers the target, Cancel aborts).
// the work and any prompts are owned here; the cursor lands on the result.
void renameActiveTo(const char *newName);

// creates a new file / folder named name in the current directory and parks the
// cursor on it. validation lives here. New File replaces an existing file only
// after a Replace / Cancel prompt and never replaces a folder; New Folder merges
// into an existing entry, which for an empty folder is just a no-op + select.
void createFile(const char *name);
void createFolder(const char *name);

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

// zips the current selection (checked rows, or the active row when none are
// checked) into name within the current directory, via the progress overlay.
// a free name zips at once; a collision with an existing file opens a Replace /
// Cancel prompt (mirrors createFile); a collision with a folder is refused.
void zipSelectionTo(const char *name);

// extracts the active row (must be a .zip file, per getAvailableActions) into a
// new subfolder named after the archive, via the progress overlay. a free name
// extracts at once; an existing subfolder merges in with a Replace All / Keep
// All prompt for file collisions inside it.
void unzipActive(void);
