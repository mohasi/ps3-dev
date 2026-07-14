#pragma once

// search-controller - owns the home screen's search mode: the results view (search-list widget), the
// search-back navigation model, and the search side-menu actions. Home wires it in initHome and
// delegates its search branches here, so the screen code stays about layout, not search state.

#include "font.h"
#include "gfx.h"
#include "audio.h"
#include "selection-actions.h"

void initSearchController(Font *font, GfxTexture sprites, Audio *clickSfx, Audio *checkSfx,
                          int listY, int rowHeight, int fontSize, uint32_t color, void (*onOptions)(void));
void termSearchController(void);

int  isSearchActive(void);     // home: choose the search view over the file list in update/draw
void launchSearch(void);       // START footer button: open the search keyboard

void updateSearchView(void);   // search active: pump the results list (it owns its own input)
int  handleSearchBack(void);   // search inactive: re-enter search on Back at the jumped-into folder; 1 if consumed
void drawSearchTitle(void);    // header slot: "Search Results" (in place of the breadcrumb)
void drawSearchResults(void);  // body slot: the results list (in place of the file list)

// search side-menu content + dispatch. home owns the shared sidepanel and asks these while search is active.
int  searchHasOptions(void);                          // results present and the walk has finished
const SelectionAction  *getSearchActions(int *count);
const SelectionSummary *getSearchSummary(void);
void dispatchSearchAction(SelectionAction action);
