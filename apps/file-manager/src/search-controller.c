// search-controller - see search-controller.h
#include "search-controller.h"
#include "widgets/search-list.h"       // the results widget + its action verbs
#include "widgets/file-list.h"         // showFileListPath / openFile / getCurrentPath
#include "widgets/footer-widget.h"     // setFooterButtonVisible
#include "file-type.h"                 // FileType / FILE_TYPE_FOLDER
#include "overlays/confirm-overlay.h"  // askConfirm / ConfirmChoice
#include "osk-input.h"                 // oskInputBegin
#include "ui/label.h"
#include "pad.h"
#include "path.h"                      // MAX_PATH_LEN
#include "string-utilities.h"          // strCopy / strEq
#include <stdio.h>                     // snprintf

#define SEARCH_QUERY_MAX 96

// search-back model: searching from folder D shows the results. Cross on a folder result jumps the
// list into that folder; Back walks up the tree normally, and once back AT that folder Back returns
// to the search (re-run fresh, so it reflects any changes). Back again leaves search and restores D.
// Cross on a file result opens it in a viewer over the still-shown results.
static int   searchActive;
static Label searchTitle;                   // "Search Results", replaces the breadcrumb while active
static char  searchRoot[MAX_PATH_LEN];      // folder the search was launched from; restored on exit
static char  searchQuery[SEARCH_QUERY_MAX]; // kept so the search can be re-run on Back
static char  jumpFolder[MAX_PATH_LEN];      // folder a result was opened into; Back here returns to search
static int   cameFromSearch;                // a folder result was opened - watch for Back at jumpFolder
static int   searchDisplaced;               // the list was moved by a jump; restore searchRoot on exit

// the actions that make sense on a set scattered across folders. copy/cut/delete act on the whole
// target set; rename acts on the highlighted row. paste/zip/create need one destination folder, which
// a search has none of; edit/play stay on Cross (see onSearchActivate).
static const SelectionAction searchActions[] = { ACTION_COPY, ACTION_CUT, ACTION_DELETE, ACTION_RENAME };

// while search is active the results list replaces the file list, and the file-list-only footer
// actions (FTP / Search) hide. Options (Triangle) stays available in both modes.
static void setSearchMode(int on)
{
   searchActive = on;
   setFooterButtonVisible(PAD_BTN_SELECT, !on);
   setFooterButtonVisible(PAD_BTN_START, !on);
}

int isSearchActive(void) { return searchActive; }

// search-list callbacks
static void onSearchActivate(const char *path, FileType type)
{
   if (type == FILE_TYPE_FOLDER) {
      showFileListPath(path, NULL);
      setSearchMode(0);
      strCopy(jumpFolder, sizeof jumpFolder, path);
      cameFromSearch = 1;
      searchDisplaced = 1;
   } else {
      openFile(path, type);   // opens a viewer over the still-shown results; closing it returns here
   }
}

static void onSearchExit(void)
{
   setSearchMode(0);
   if (searchDisplaced) { showFileListPath(searchRoot, NULL); searchDisplaced = 0; }
   cameFromSearch = 0;
}

static void onSearchQuery(const char *text)
{
   if (!text || !text[0]) return;   // cancelled or empty
   strCopy(searchRoot, sizeof searchRoot, getCurrentPath());
   strCopy(searchQuery, sizeof searchQuery, text);
   cameFromSearch = 0;
   searchDisplaced = 0;
   setSearchMode(1);
   beginSearch(searchRoot, searchQuery);
}

void launchSearch(void) { oskInputBegin("Search", "", onSearchQuery); }

// side-menu actions
static void onSearchDeleteConfirmed(ConfirmChoice choice) { if (choice == CONFIRM_CROSS) deleteSearchSelection(); }
static void onSearchRenameDone(const char *text)          { if (text) applySearchRename(text); }

void dispatchSearchAction(SelectionAction action)
{
   switch (action) {
      case ACTION_COPY: copySearchSelection(); break;
      case ACTION_CUT:  cutSearchSelection();  break;

      case ACTION_DELETE: {
         int n = getSearchTargetCount();
         char message[64];
         snprintf(message, sizeof message, "Are you sure you want to delete %d %s?", n, n == 1 ? "item" : "items");
         askConfirm("Delete Confirmation", message, "Yes", NULL, "No", onSearchDeleteConfirmed);
         break;
      }

      case ACTION_RENAME: {
         const char *name = getSearchActiveName();
         if (name) oskInputBegin("Rename", name, onSearchRenameDone);
         break;
      }

      default: break;
   }
}

int searchHasOptions(void) { return getSearchResultCount() > 0 && !isSearchRunning(); }
const SelectionAction  *getSearchActions(int *count) { *count = (int)(sizeof searchActions / sizeof searchActions[0]); return searchActions; }
const SelectionSummary *getSearchSummary(void) { return getSearchSelectionSummary(); }

// update / draw
void updateSearchView(void) { updateSearchList(); }   // the widget owns all of its own input

int handleSearchBack(void)
{
   if (cameFromSearch && isPadButtonPressed(PAD_BTN_CIRCLE) && strEq(getCurrentPath(), jumpFolder)) {
      setSearchMode(1);
      beginSearch(searchRoot, searchQuery);   // re-run fresh so it reflects any changes
      cameFromSearch = 0;
      return 1;
   }
   return 0;
}

void drawSearchTitle(void)   { drawLabel(&searchTitle); }
void drawSearchResults(void) { drawSearchList(); }

void initSearchController(Font *font, GfxTexture sprites, Audio *clickSfx, Audio *checkSfx,
                          int listY, int rowHeight, int fontSize, uint32_t color, void (*onOptions)(void))
{
   initSearchList(font, sprites, clickSfx, checkSfx, listY, rowHeight, fontSize, color, onSearchActivate, onSearchExit, onOptions);
   initLabel(&searchTitle, font, 70, 128, AUTO, AUTO, 24, color, TEXT_NOWRAP, "Search Results");
}

void termSearchController(void)
{
   termSearchList();
   freeLabel(&searchTitle);
}
