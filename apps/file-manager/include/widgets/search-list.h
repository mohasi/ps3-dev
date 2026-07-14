#pragma once

// search-list - a results view that mirrors file-list but shows matches from a recursive name
// search rooted at one folder. It swaps into the home screen in place of file-list while a search
// is active. Rows can be marked; Cross opens a file / navigates into a folder, Circle leaves search,
// and an Options side-menu (copy / cut / delete / rename) acts on the marked set - see home.

#include "font.h"
#include "gfx.h"
#include "audio.h"
#include "file-type.h"
#include "selection-actions.h"   // SelectionSummary for the options side-menu

// Cross on a result. the caller decides what to do by type: a folder opens in the file list (and
// search is left), a file opens in its viewer over the still-active search. `path` is the full path.
typedef void (*SearchActivate)(const char *path, FileType type);

// Circle at the results: leave search. the caller restores whatever was showing before.
typedef void (*SearchExit)(void);

// Triangle at the results: open the options side-menu. the search list owns all of its own input, so
// this cannot ride the file-list footer buttons (doing so double-dispatched Cross to the hidden list).
typedef void (*SearchOptions)(void);

void initSearchList(Font *font, GfxTexture spritesheet, Audio *clickSfx, Audio *checkSfx, int y, int rowHeight,
                    int fontSize, uint32_t color, SearchActivate onActivate, SearchExit onExit, SearchOptions onOptions);
void termSearchList(void);

// starts a case-insensitive recursive name search under `root` for `query`, replacing any previous
// results, and kicks the background walker. entering search mode is the caller's job (see home).
void beginSearch(const char *root, const char *query);

void updateSearchList(void);   // input + poll the background walker
void drawSearchList(void);     // headers, rows, and a status line (count / searching / no matches)

// options side-menu support. the widget owns the actions (like file-list's cutSelection/deleteSelection);
// home only builds the panel and drives the delete confirm / rename keyboard. the target set is the
// checked rows, or the highlighted row when none are checked.
int getSearchResultCount(void);   // total matches; 0 means there is nothing to act on
int isSearchRunning(void);        // 1 while the walker is still appending - block edits until it stops
int getSearchTargetCount(void);   // size of the target set, for the delete-confirm wording
const SelectionSummary *getSearchSelectionSummary(void);   // header describing the target set
const char *getSearchActiveName(void);   // highlighted row's base name, for a rename prompt (NULL if empty)

void copySearchSelection(void);   // gather the target set onto the clipboard (copy)
void cutSearchSelection(void);    // gather the target set onto the clipboard (cut)
void deleteSearchSelection(void); // delete the target set (runs the progress dialog), then refresh the rows

void applySearchRename(const char *newName);   // rename the highlighted row's file and update its stored path
