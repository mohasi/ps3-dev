// search-list - see widgets/search-list.h
#include "widgets/search-list.h"
#include "widgets/list-row-chrome.h"   // shared drawListRowSeparator / drawListRowHighlight
#include "gfx.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/checkbox.h"
#include "file-type.h"
#include "sprite-regions.h"
#include "theme.h"
#include "button-repeat.h"
#include "tree-walk.h"
#include "clipboard.h"           // begin/addToClipboard, isOnClipboard - copy/cut + ghost rows
#include "delete.h"              // begin/addToDelete/runDelete - the delete verb
#include "overlays/progress-overlay.h"   // startProgress - runs the delete with a progress dialog
#include "vfs.h"                 // openDir/readDir/statPath, getBaseName/getParentPath, MAX_PATH_LEN
#include "thread.h"
#include <sys/sys_time.h>        // sys_time_get_system_time for the hold-to-select-all timer
#include "string-utilities.h"
#include "format.h"             // formatSize
#include "audio.h"
#include "pad.h"
#include "dbg.h"
#include <stdlib.h>
#include <stdio.h>

#define PAGE_SIZE      9
#define MAX_RESULTS  400   // bounds memory (~400 * sizeof(SearchEntry)); a truncated search is logged
#define QUERY_MAX      96
#define MARKED_ROW_ALPHA 0x80   // 50% opacity for rows pending a cut or copy (matches file-list)

// columns: checkbox, type icon, name, full location, type, size. no modified/permissions - the
// location is the point of a search result and takes the room they would need.
#define CHECKBOX_X     71
#define ICON_X        120
#define NAME_HEADER_X  88   // "Name" header sits over the checkbox/icon zone, matching the file list
#define NAME_X        177
#define NAME_W        470
#define LOCATION_X  680
#define LOCATION_W  840
#define TYPE_X     1560
#define TYPE_W      140    // same as file-list
#define SIZE_X     1750   // 190px past TYPE_X, the same type->size gap as file-list
#define SIZE_W      130    // same as file-list
#define COLUMN_HEADER_Y 212
#define STATUS_X     55
#define STATUS_Y    953

typedef struct {
   char     path[MAX_PATH_LEN];   // full path of the match; name and location are derived from it
   uint64_t size;
   FileType type;
   int      checked;
} SearchEntry;

static SearchEntry  *results;         // MAX_RESULTS entries, allocated while a search is active
static volatile int  resultCount;     // published by the worker; append-only, read by the main thread
static volatile int  cancel;          // raised to stop the worker
static volatile int  searching;       // 1 while the worker is walking
static volatile int  truncated;       // hit MAX_RESULTS (worker-written, main-read like its siblings)
static int           threadActive;
static sys_ppu_thread_t workerTid;

static char root[MAX_PATH_LEN];
static char query[QUERY_MAX];

static int selectedIndex;
static int scrollOffset;
static int armed;   // 0 on the frame search opens, so the keyboard's confirm press isn't read as input
static int lastRenderedCount;         // rebuild row labels when the visible window changes
static int lastRenderedScroll;

static Audio     *clickSfx, *checkSfx;
static int        listY, listRowHeight;   // row geometry, read by draw()
static SearchActivate activateCb;
static SearchExit     exitCb;
static SearchOptions  optionsCb;

static Image     fileIcons[FILE_TYPE_COUNT];
static Label     nameLabels[PAGE_SIZE], locationLabels[PAGE_SIZE], typeLabels[PAGE_SIZE], sizeLabels[PAGE_SIZE];
static Checkbox  checkboxes[PAGE_SIZE];
static Label     nameHeader, locationHeader, typeHeader, sizeHeader, statusLabel;

static ButtonRepeat scrollUpRepeat, scrollDownRepeat;

// ============================================================================
// background search walk (mirrors folder-sizer's iterative, cancellable walk)
// ============================================================================

// case-insensitive substring test: is needle contained in haystack, ignoring ASCII case?
static int containsFold(const char *haystack, const char *needle)
{
   if (!needle[0]) return 1;
   for (const char *h = haystack; *h; h++) {
      const char *a = h, *b = needle;
      while (*a && *b && toLowerChar(*a) == toLowerChar(*b)) { a++; b++; }
      if (!*b) return 1;
   }
   return 0;
}

// appends one match; single-producer (the worker). the store barrier below pairs with the load
// barrier in drawSearchList so the slot's bytes are visible before the bumped count is. returns 0
// once the cap is hit.
static int appendResult(const char *path, uint64_t size, FileType type)
{
   int n = resultCount;
   if (n >= MAX_RESULTS) { truncated = 1; return 0; }
   strCopy(results[n].path, MAX_PATH_LEN, path);
   results[n].size    = size;
   results[n].type    = type;
   results[n].checked = 0;
   __sync_synchronize();
   resultCount = n + 1;
   return 1;
}

// walkTree visitor: append every entry whose name matches the query; stop the walk at the cap. only
// a match is stat'd (for its size) - non-matches, the vast majority, cost nothing beyond the readdir.
static WalkResult matchVisit(const char *fullPath, const char *name, VfsEntryType type, void *ctx)
{
   (void)ctx;
   if (!containsFold(name, query)) return WALK_CONTINUE;

   int isDir = type == VFS_ENTRY_DIR;
   uint64_t size = 0;
   if (!isDir) { VfsStat st; if (statPath(fullPath, &st) == 0) size = st.size; }
   if (!appendResult(fullPath, size, classifyFileType(name, isDir)))
      return WALK_STOP;
   return WALK_CONTINUE;
}

// removable volumes appear at the root as dev_usb* / ntfs* / exfat* - searched last so the internal
// drive's matches come up first.
static int isRemovableTop(const char *name)
{
   return startsWith(name, "dev_usb") || startsWith(name, "ntfs") || startsWith(name, "exfat");
}

// runs the walk. from the filesystem root, top-level volumes are walked internal-first / removable-
// last; otherwise the single subtree is walked directly.
static void runSearch(void)
{
   if (!strEq(root, "/")) { walkTree(root, matchVisit, NULL, &cancel); return; }

   char names[32][64];
   int  n = 0;
   VfsDir dir;
   if (openDir("/", &dir) == 0) {
      char name[256];
      while (n < 32 && readDir(&dir, name, sizeof name, NULL) == 1) {
         if (name[0] == '.') continue;
         strCopy(names[n++], sizeof names[0], name);
      }
      closeDir(&dir);
   }

   for (int removable = 0; removable <= 1 && !cancel && resultCount < MAX_RESULTS; removable++)
      for (int i = 0; i < n && !cancel && resultCount < MAX_RESULTS; i++) {
         if (isRemovableTop(names[i]) != removable) continue;
         char top[MAX_PATH_LEN];
         top[0] = '/';
         strCopy(top + 1, sizeof top - 1, names[i]);
         walkTree(top, matchVisit, NULL, &cancel);
      }
}

static void worker(uint64_t arg)
{
   (void)arg;
   runSearch();
   searching = 0;
   exitThread();
}

// stops the worker but keeps whatever was found - the Circle-to-cancel on the progress panel.
static void pauseSearch(void)
{
   cancel = 1;
   if (threadActive) { joinThread(workerTid); threadActive = 0; }
   searching = 0;
}

// stops the worker and releases the results buffer, so nothing is resident between searches.
static void stopSearch(void)
{
   pauseSearch();
   free(results);
   results = NULL;
   resultCount = 0;
}

// status line + completion test polled by the busy progress overlay while the walk runs.
static const char *searchStatus(void)
{
   static char text[32];
   int length = appendUint64(text, sizeof text, 0, (uint64_t)resultCount);
   appendStr(text, sizeof text, &length, " found");
   text[length] = '\0';
   return text;
}
static int searchFinished(void) { return !searching; }

void beginSearch(const char *searchRoot, const char *searchQuery)
{
   stopSearch();   // clear any previous search first

   // calloc (not malloc): a slot the worker hasn't published yet then reads back as a zeroed row
   // (type 0 = a valid icon index, empty path) if the draw thread races ahead of the count, instead
   // of garbage that could index fileIcons[] out of bounds.
   results = (SearchEntry *)calloc(MAX_RESULTS, sizeof(SearchEntry));
   if (!results) { logError("[search] out of memory for results\n"); return; }

   strCopy(root, sizeof root, searchRoot);
   strCopy(query, sizeof query, searchQuery);
   resultCount = 0;
   selectedIndex = 0;
   scrollOffset = 0;
   armed = 0;
   truncated = 0;
   lastRenderedCount = -1;
   lastRenderedScroll = -1;

   cancel = 0;
   searching = 1;
   threadActive = (spawnJoinableThread(&workerTid, worker, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "search") == 0);
   if (!threadActive) { searching = 0; logError("[search] worker spawn failed\n"); return; }

   // the walk is modal: the busy progress overlay shows the live count + cancel while it runs.
   startBusyProgress("Searching...", searchStatus, searchFinished, pauseSearch, NULL);
}

// ============================================================================
// input
// ============================================================================

static void scrollToSelected(void)
{
   if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
   else if (selectedIndex >= scrollOffset + PAGE_SIZE) scrollOffset = selectedIndex - PAGE_SIZE + 1;
}

// Cross on a result: hand the full path and type to the caller. a folder leaves search entirely (the
// caller re-runs the search on Back, so the results are freed here); a file opens in a viewer over
// the still-shown results (kept). only reachable once the walk has finished, so the worker is idle.
static void activateSelected(void)
{
   if (selectedIndex < 0 || selectedIndex >= resultCount) return;

   char path[MAX_PATH_LEN];
   FileType type = results[selectedIndex].type;
   strCopy(path, sizeof path, results[selectedIndex].path);   // copy before we (maybe) free the buffer

   playAudioOnce(clickSfx);
   if (type == FILE_TYPE_FOLDER) stopSearch();   // freed; the caller rescans on Back
   activateCb(path, type);
}

#define SQUARE_HOLD_MS 400   // tap toggles the focused row; holding this long toggles the whole set

static void setAllChecked(int value) { for (int i = 0; i < resultCount; i++) results[i].checked = value; }

static int allResultsChecked(void)
{
   for (int i = 0; i < resultCount; i++) if (!results[i].checked) return 0;
   return resultCount > 0;
}

// square: tap toggles the focused row, hold toggles the whole result set. hold fires once at the
// threshold and suppresses the would-be tap on release (mirrors file-list's handleCheckInput).
static void handleCheckInput(int hasSelection)
{
   static uint64_t pressedUs;
   static int      holdFired;

   if (isPadButtonPressed(PAD_BTN_SQUARE)) { pressedUs = sys_time_get_system_time(); holdFired = 0; return; }

   if (isPadButtonHeld(PAD_BTN_SQUARE) && !holdFired && resultCount > 0 &&
       sys_time_get_system_time() - pressedUs >= SQUARE_HOLD_MS * 1000ULL) {
      setAllChecked(!allResultsChecked());
      holdFired = 1;
      playAudioOnce(checkSfx);
      return;
   }

   if (isPadButtonReleased(PAD_BTN_SQUARE) && !holdFired && hasSelection) {
      results[selectedIndex].checked = !results[selectedIndex].checked;
      playAudioOnce(checkSfx);
   }
}

void updateSearchList(void)
{
   if (!armed) { armed = 1; return; }   // swallow the keyboard-confirm press that opened this search
   if (searching) return;               // the busy progress overlay owns the walk (status + cancel)

   int count = resultCount;

   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { optionsCb(); return; }
   if (isPadButtonPressed(PAD_BTN_CIRCLE))   { stopSearch(); exitCb(); return; }
   if (isPadButtonPressed(PAD_BTN_CROSS))    { activateSelected(); return; }

   handleCheckInput(count > 0 && selectedIndex >= 0 && selectedIndex < count);

   if (isRepeatDue(&scrollDownRepeat, getPadButtonState(PAD_BTN_DOWN)) && count > 0) {
      selectedIndex = (selectedIndex + 1) % count;             // wraps bottom -> top, like file-list
      scrollToSelected();
      playAudioOnce(clickSfx);
   } else if (isRepeatDue(&scrollUpRepeat, getPadButtonState(PAD_BTN_UP)) && count > 0) {
      selectedIndex = (selectedIndex - 1 + count) % count;     // wraps top -> bottom
      scrollToSelected();
      playAudioOnce(clickSfx);
   }
}

// ============================================================================
// options side-menu (home builds the panel; these expose and mutate the target set)
// ============================================================================

// target set = the checked rows, or the highlighted row when none are checked (mirrors file-list).
static int countChecked(void)
{
   int n = 0;
   for (int i = 0; i < resultCount; i++) if (results[i].checked) n++;
   return n;
}

static int isTargeted(int i, int checkedCount) { return checkedCount > 0 ? results[i].checked : i == selectedIndex; }

int getSearchResultCount(void) { return resultCount; }
int isSearchRunning(void) { return searching; }

// visits each targeted path once (size exact for files, 0 for folders); returns the count. NULL just
// counts. internal now - the copy/cut/delete verbs below are the widget's public action surface.
typedef void (*SearchTargetVisit)(const char *path, uint64_t size, int exact, void *ctx);

static int visitSearchTargets(SearchTargetVisit visit, void *ctx)
{
   int checkedCount = countChecked();
   int n = 0;
   for (int i = 0; i < resultCount; i++) {
      if (!isTargeted(i, checkedCount)) continue;
      if (visit) visit(results[i].path, results[i].size, results[i].type != FILE_TYPE_FOLDER, ctx);
      n++;
   }
   return n;
}

int getSearchTargetCount(void) { return visitSearchTargets(NULL, NULL); }

const char *getSearchActiveName(void)
{
   if (selectedIndex < 0 || selectedIndex >= resultCount) return NULL;
   return getBaseName(results[selectedIndex].path);
}

const SelectionSummary *getSearchSelectionSummary(void)
{
   static SelectionSummary summary;
   static char title[128], subtitle[32];

   int count = getSearchTargetCount();
   if (count > 1) {
      snprintf(title, sizeof title, "%d items", count);
      subtitle[0] = '\0';
      summary.icon = spriteRegions[SPRITE_GENERIC_MULTI];
   } else {
      // the single target: the lone checked row, else the highlighted one.
      int idx = selectedIndex;
      if (countChecked() == 1) for (int i = 0; i < resultCount; i++) if (results[i].checked) { idx = i; break; }
      strCopy(title, sizeof title, getBaseName(results[idx].path));
      strCopy(subtitle, sizeof subtitle, getFileTypeName(results[idx].type));
      summary.icon = spriteRegions[getFileTypeSprite(results[idx].type)];
   }
   summary.title    = title;
   summary.subtitle = subtitle;
   summary.detail   = "";
   return &summary;
}

// drops every result row whose path no longer exists, then fixes up the cursor. re-stats ALL rows,
// not just the targeted ones: deleting or renaming a folder takes its descendants with it, and those
// child rows were never targeted, so a targeted-only check would leave them behind pointing at gone
// paths. shared by delete and folder-rename.
static void dropMissingResults(void)
{
   VfsStat st;
   int write = 0;
   for (int read = 0; read < resultCount; read++) {
      if (statPath(results[read].path, &st) != 0) continue;   // gone: drop
      if (write != read) results[write] = results[read];
      write++;
   }
   resultCount = write;
   if (selectedIndex >= resultCount) selectedIndex = resultCount > 0 ? resultCount - 1 : 0;
   scrollToSelected();
   lastRenderedCount = -1;   // force the visible rows to rebuild
}

// action verbs - the widget owns these (mirrors file-list's clipboardFromSelection/deleteSelection).
static void addPathToClipboard(const char *path, uint64_t size, int exact, void *ctx) { (void)ctx; addToClipboard(path, size, exact); }
static void addPathToDelete(const char *path, uint64_t size, int exact, void *ctx)    { (void)ctx; addToDelete(path, size, exact); }

void copySearchSelection(void) { beginClipboard(CLIP_COPY); visitSearchTargets(addPathToClipboard, NULL); }
void cutSearchSelection(void)  { beginClipboard(CLIP_CUT);  visitSearchTargets(addPathToClipboard, NULL); }

// main-thread finisher: a delete invalidates any pending cut/copy, then drop the rows that are gone.
static void onDeleteFinished(int cancelled) { (void)cancelled; clearClipboard(); dropMissingResults(); }

void deleteSearchSelection(void)
{
   beginDelete();
   if (visitSearchTargets(addPathToDelete, NULL) == 0) return;
   startProgress("Deleting...", "Please wait while the selected items are deleted.", runDelete, onDeleteFinished);
}

// rename the highlighted row's file in place; refuse a name collision to keep search simple.
void applySearchRename(const char *newName)
{
   if (selectedIndex < 0 || selectedIndex >= resultCount || !isValidFileName(newName)) return;

   char parent[MAX_PATH_LEN], newPath[MAX_PATH_LEN];
   getParentPath(results[selectedIndex].path, parent, sizeof parent);
   joinPath(newPath, sizeof newPath, parent, newName);
   if (fileExists(newPath)) return;                          // don't clobber an existing file
   if (renamePath(results[selectedIndex].path, newPath) != 0) return;

   int isDir = results[selectedIndex].type == FILE_TYPE_FOLDER;
   strCopy(results[selectedIndex].path, MAX_PATH_LEN, newPath);
   results[selectedIndex].type = classifyFileType(newName, isDir);   // extension may have changed the type
   if (isDir) dropMissingResults();   // a folder rename orphans child rows left under the old path
   else       lastRenderedCount = -1;
}

// ============================================================================
// draw
// ============================================================================

static void rebuildRows(int count)
{
   char parent[MAX_PATH_LEN], sizeText[24];
   for (int i = 0; i < PAGE_SIZE; i++) {
      int idx = scrollOffset + i;
      if (idx >= count) {
         setLabelText(&nameLabels[i], "");
         setLabelText(&locationLabels[i], "");
         setLabelText(&typeLabels[i], "");
         setLabelText(&sizeLabels[i], "");
         continue;
      }
      setLabelText(&nameLabels[i], getBaseName(results[idx].path));
      getParentPath(results[idx].path, parent, sizeof parent);
      setLabelText(&locationLabels[i], parent);
      setLabelText(&typeLabels[i], getFileTypeName(results[idx].type));

      if (results[idx].type == FILE_TYPE_FOLDER) {
         setLabelText(&sizeLabels[i], EM_DASH);
      } else {
         formatSize(results[idx].size, sizeText);
         setLabelText(&sizeLabels[i], sizeText);
      }
   }
   lastRenderedCount = count;
   lastRenderedScroll = scrollOffset;
}

// bottom-left summary, shown once the walk has finished.
static void drawStatus(int count)
{
   char text[48];
   if (count == 0)     snprintf(text, sizeof text, "No matches");
   else if (truncated) snprintf(text, sizeof text, "%d results (first %d shown)", count, MAX_RESULTS);
   else                snprintf(text, sizeof text, "%d results", count);
   setLabelText(&statusLabel, text);
   drawLabel(&statusLabel);
}

void drawSearchList(void)
{
   int count = resultCount;
   __sync_synchronize();   // load barrier: pair with appendResult so a published slot's bytes are visible
   if (count != lastRenderedCount || scrollOffset != lastRenderedScroll) rebuildRows(count);

   drawLabel(&nameHeader);
   drawLabel(&locationHeader);
   drawLabel(&typeHeader);
   drawLabel(&sizeHeader);

   for (int i = 0; i < PAGE_SIZE; i++) {
      int idx = scrollOffset + i;
      if (idx >= count) break;

      int rowY = listY + i * listRowHeight;
      if (i > 0 && idx != selectedIndex && idx - 1 != selectedIndex) drawListRowSeparator(rowY);
      if (idx == selectedIndex) drawListRowHighlight(rowY, listRowHeight);

      // rows on the clipboard (cut or copy) render ghosted, like file-list
      int alpha = isOnClipboard(results[idx].path) ? MARKED_ROW_ALPHA : 0xFF;

      drawCheckboxAlpha(&checkboxes[i], results[idx].checked, alpha);

      int iconY = rowY + (listRowHeight - 43) / 2;
      moveImage(&fileIcons[results[idx].type], ICON_X, iconY);
      drawImageAlpha(&fileIcons[results[idx].type], alpha);

      drawLabelAlpha(&nameLabels[i], alpha);
      drawLabelAlpha(&locationLabels[i], alpha);
      drawLabelAlpha(&typeLabels[i], alpha);
      drawLabelAlpha(&sizeLabels[i], alpha);
   }

   if (!searching) drawStatus(count);   // while the walk runs, the busy progress overlay covers this
}

// ============================================================================
// setup
// ============================================================================

void initSearchList(Font *font, GfxTexture sprites, Audio *click, Audio *check, int y, int rowHeight,
                    int fontSize, SearchActivate onActivate, SearchExit onExit, SearchOptions onOptions)
{
   uint32_t color = activeTheme->textPrimary;   // headers + result-row labels all use the primary text colour
   clickSfx      = click;
   checkSfx      = check;
   listY         = y;
   listRowHeight = rowHeight;
   activateCb    = onActivate;
   exitCb        = onExit;
   optionsCb     = onOptions;

   for (int t = 0; t < FILE_TYPE_COUNT; t++)
      initImage(&fileIcons[t], sprites, 0, 0, 35, 43, spriteRegions[getFileTypeSprite(t)], GFX_FILTER_LINEAR);

   initLabel(&nameHeader,     font, NAME_HEADER_X, COLUMN_HEADER_Y, NAME_W,  AUTO, fontSize, color, TEXT_NOWRAP, "Name");
   initLabel(&locationHeader, font, LOCATION_X, COLUMN_HEADER_Y, LOCATION_W, AUTO, fontSize, color, TEXT_NOWRAP, "Location");
   initLabel(&typeHeader,     font, TYPE_X,     COLUMN_HEADER_Y, TYPE_W,     AUTO, fontSize, color, TEXT_NOWRAP, "Type");
   initLabel(&sizeHeader,     font, SIZE_X,     COLUMN_HEADER_Y, SIZE_W,     AUTO, fontSize, color, TEXT_NOWRAP, "Size");
   initLabel(&statusLabel,    font, STATUS_X,   STATUS_Y,        400,        AUTO, 20,       color, TEXT_NOWRAP, "");

   for (int i = 0; i < PAGE_SIZE; i++) {
      int ry = y + i * rowHeight;
      int cy = ry + (rowHeight - 25) / 2;
      initLabel(&nameLabels[i],     font, NAME_X,     ry + 25, NAME_W,     AUTO, fontSize, color, TEXT_NOWRAP_ELLIPSIS, NULL);
      initLabel(&locationLabels[i], font, LOCATION_X, ry + 25, LOCATION_W, AUTO, fontSize, color, TEXT_NOWRAP_ELLIPSIS, NULL);
      initLabel(&typeLabels[i],     font, TYPE_X,     ry + 25, TYPE_W,     AUTO, fontSize, color, TEXT_NOWRAP,          NULL);
      initLabel(&sizeLabels[i],     font, SIZE_X,     ry + 25, SIZE_W,     AUTO, fontSize, color, TEXT_NOWRAP,          NULL);
      initCheckbox(&checkboxes[i], CHECKBOX_X, cy, 25, activeTheme->checkBorder, activeTheme->checkFill);
   }
}

// recolours every persistent label + checkbox for a live theme switch (chrome is drawn live).
void rethemeSearchList(void)
{
   for (int i = 0; i < PAGE_SIZE; i++) {
      setLabelColor(&nameLabels[i],     activeTheme->textPrimary);
      setLabelColor(&locationLabels[i], activeTheme->textPrimary);
      setLabelColor(&typeLabels[i],     activeTheme->textPrimary);
      setLabelColor(&sizeLabels[i],     activeTheme->textPrimary);
      checkboxes[i].borderColor = activeTheme->checkBorder;
      checkboxes[i].fillColor   = activeTheme->checkFill;
   }
   setLabelColor(&nameHeader,     activeTheme->textPrimary);
   setLabelColor(&locationHeader, activeTheme->textPrimary);
   setLabelColor(&typeHeader,     activeTheme->textPrimary);
   setLabelColor(&sizeHeader,     activeTheme->textPrimary);
   setLabelColor(&statusLabel,    activeTheme->textPrimary);
}

void termSearchList(void)
{
   stopSearch();
   for (int i = 0; i < PAGE_SIZE; i++) {
      freeLabel(&nameLabels[i]);
      freeLabel(&locationLabels[i]);
      freeLabel(&typeLabels[i]);
      freeLabel(&sizeLabels[i]);
   }
   freeLabel(&nameHeader);
   freeLabel(&locationHeader);
   freeLabel(&typeHeader);
   freeLabel(&sizeHeader);
   freeLabel(&statusLabel);
}
