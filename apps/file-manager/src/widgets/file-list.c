// file-list - scrollable directory listing widget
#include "widgets/file-list.h"
#include "app.h"   // requestAppExit: Circle at root offers to quit
#include "ui/label.h"
#include "ui/image.h"
#include "ui/slice.h"
#include "ui/breadcrumb.h"
#include "ui/checkbox.h"
#include "widgets/footer-widget.h"
#include "sprite-regions.h"
#include "folder-sizer.h"
#include "file-type.h"
#include "clipboard.h"
#include "paste.h"
#include "delete.h"
#include "zip-task.h"
#include "unzip-task.h"
#include "vfs.h"
#include "file-task.h"
#include "dynarray.h"
#include "overlays/progress-overlay.h"
#include "overlays/confirm-overlay.h"
#include "overlays/image-viewer-overlay.h"
#include "overlays/audio-player-overlay.h"
#include "overlays/video-player-overlay.h"
#include "overlays/text-editor-overlay.h"
#include "overlays/hex-viewer-overlay.h"
#include "image-loader.h"
#include "pad.h"
#include <stdio.h>
#include "audio.h"
#include "button-repeat.h"
#include "string-utilities.h"
#include <string.h>
#include <stdlib.h>
#include <sys/sys_time.h>

#define FILE_LIST_PAGE_SIZE 9
#define NAME_LEN           256
#define INITIAL_CAPACITY   256
#define HISTORY_MAX        16
#define HIGHLIGHT_CAP      7    // highlight sprite (16x16) 9-slice corner cap

typedef struct {
   char name[NAME_LEN];
   uint64_t size;
   uint64_t modified;
   uint32_t mode;    // st_mode-style permission bits (0 when the entry couldn't be stat'ed)
   int fileCount;    // 1 for files; recursive count for folders (valid when sized)
   FileType type;
   int checked;
   int sized;        // folders: 0 until folder-sizer visits it
   int approx;       // sized but the walker hit its budget; size/fileCount are a lower bound
   int isUsb;        // root listing only: a removable (USB) device, badged with the USB icon
} FileEntry;

static FileEntry *entries;
static int entryCount;
static int entryCapacity;
static int selectedIndex;
static int scrollOffset;

static Label labels[FILE_LIST_PAGE_SIZE];
static Label sizeLabels[FILE_LIST_PAGE_SIZE];
static Label typeLabels[FILE_LIST_PAGE_SIZE];
static Label modifiedLabels[FILE_LIST_PAGE_SIZE];
static Label permissionLabels[FILE_LIST_PAGE_SIZE];
static Label counterLabel;

// column layout: one x/width per column, shared by the header row and the row labels
#define COLUMN_TYPE_X        1080
#define COLUMN_TYPE_W         140
#define COLUMN_SIZE_X        1270
#define COLUMN_SIZE_W         130
#define COLUMN_MODIFIED_X    1440
#define COLUMN_MODIFIED_W     190
#define COLUMN_PERMISSIONS_X 1680
#define COLUMN_PERMISSIONS_W  175

// column headers, rendered here rather than baked into background.png so columns can be moved
// and added freely (Name sits over the checkbox/icon block, the rest match the row columns)
#define COLUMN_HEADER_Y 198
static const struct { int x; const char *text; } COLUMN_HEADERS[] = {
   { 88, "Name" }, { COLUMN_TYPE_X, "Type" }, { COLUMN_SIZE_X, "Size" },
   { COLUMN_MODIFIED_X, "Modified" }, { COLUMN_PERMISSIONS_X, "Permissions" }
};
#define COLUMN_HEADER_COUNT ((int)(sizeof COLUMN_HEADERS / sizeof COLUMN_HEADERS[0]))
static Label columnHeaderLabels[COLUMN_HEADER_COUNT];
static int listY, listRowHeight;
static int labelsStale;
static char currentPath[MAX_PATH_LEN];
static int selectionHistory[HISTORY_MAX];
static int scrollHistory[HISTORY_MAX];
static int historyDepth;
static ButtonRepeat scrollRepeat;

// topmost deleted row, remembered during the gather so the cursor can land
// just above it once the listing refreshes. the delete set itself lives in the
// delete module (see delete.h); the listing only gathers into it.
static int delTopmost;

#define MARKED_ROW_ALPHA 0x80   // 50% opacity for rows pending a cut or copy

static Checkbox checkboxes[FILE_LIST_PAGE_SIZE];
static Image fileIcons[FILE_TYPE_COUNT];
static Image usbIcon;   // badge composited onto USB device folders at root
static Slice separators[FILE_LIST_PAGE_SIZE];
static NineSlice hover;
static Breadcrumb *breadcrumb;
static Audio *clickSfx;
static Audio *checkSfx;

// folder-sizer generation counter: incremented on every loadDir so stale
// applyResult calls from a cancelled worker that already passed its cancel
// check don't land on the wrong entry in the new listing.
static int sizerGeneration;

// folder-sizer source: lets the background walker visit unsized folders in
// this directory and write results back into the matching entry.
static int sizerCount(void) { return entryCount; }

static int sizerNeedsSizing(int i, char *outPath, int cap, int *outGeneration)
{
   if (i < 0 || i >= entryCount) return 0;
   if (entries[i].sized) return 0;
   if (entries[i].type != FILE_TYPE_FOLDER) return 0;
   joinPath(outPath, cap, currentPath, entries[i].name);
   *outGeneration = sizerGeneration;
   return 1;
}

static void sizerApplyResult(int i, uint64_t bytes, int files, int approx, int generation)
{
   if (generation != sizerGeneration) return;  // stale result from a cancelled worker
   if (i < 0 || i >= entryCount) return;
   entries[i].size      = bytes;
   entries[i].fileCount = files;
   entries[i].approx    = approx;
   entries[i].sized     = 1;
   labelsStale          = 1;
}

static const FolderSizeCallbacks sizerCallbacks = { sizerCount, sizerNeedsSizing, sizerApplyResult };

// folders before files, then case-insensitive by name. insertion sort:
// directory listings are small and already mostly-sorted on most fs's.
static int entryLess(const FileEntry *a, const FileEntry *b)
{
   int aIsDir = a->type == FILE_TYPE_FOLDER;
   int bIsDir = b->type == FILE_TYPE_FOLDER;
   if (aIsDir != bIsDir) return aIsDir > bIsDir;
   return strCmpICase(a->name, b->name) < 0;
}

static void sortEntries(void)
{
   for (int i = 1; i < entryCount; i++) {
      FileEntry key = entries[i];
      int j = i - 1;
      while (j >= 0 && entryLess(&key, &entries[j])) {
         entries[j + 1] = entries[j];
         j--;
      }
      entries[j + 1] = key;
   }
}

// At root the listing shows storage devices as folders; this flags the removable (USB)
// ones for the badge. exFAT/NTFS/ext mounts are USB-only; FAT32 USB appears under cellFs
// as dev_usbNNN. Only meaningful at root - files in a folder never set this.
static int isUsbDevice(const char *name, const char *fullPath)
{
   if (getScheme(fullPath) != VFS_SCHEME_CELLFS) return 1;
   return startsWith(name, "dev_usb");
}

// fills entry from a single stat() of the full path. size and modified time
// come from that one stat call; directories still defer their recursive size
// to the background folder-sizer (sized=0). unreadable entries are kept as a
// generic 0-byte placeholder so the row still shows up rather than dropping.
static void populateEntry(FileEntry *e, const char *name, const char *fullPath)
{
   strCopy(e->name, NAME_LEN, name);
   e->checked   = 0;
   e->fileCount = 0;
   e->approx    = 0;
   e->modified  = 0;
   e->mode      = 0;
   // root listing only: badge removable devices (folders) with the USB icon
   int atRoot = currentPath[0] == '/' && currentPath[1] == '\0';
   e->isUsb = atRoot && isUsbDevice(name, fullPath);

   VfsStat st;
   if (statPath(fullPath, &st) != 0) {
      e->type      = FILE_TYPE_GENERIC;
      e->size      = 0;
      e->fileCount = 1;
      e->sized     = 1;
      return;
   }

   int isDir = st.isDir;
   e->type = classifyFileType(name, isDir);
   e->modified = st.mtime;
   e->mode = st.mode;
   if (isDir) {
      e->size  = 0;
      e->sized = 0;  // folder-sizer fills this in
   } else {
      e->size      = st.size;
      e->fileCount = 1;
      e->sized     = 1;
   }
}

// remembers the cursor/scroll position of the current directory so circle
// can restore it when popping back. beyond HISTORY_MAX depth, drops the
// oldest entry and shifts everything down so the most recent N levels stay
// valid. this keeps the history aligned with the actual directory stack.
static void pushNavHistory(void)
{
   if (historyDepth >= HISTORY_MAX) {
      // shift out the oldest entry to make room
      for (int i = 0; i < HISTORY_MAX - 1; i++) {
         selectionHistory[i] = selectionHistory[i + 1];
         scrollHistory[i]    = scrollHistory[i + 1];
      }
      historyDepth = HISTORY_MAX - 1;
   }
   selectionHistory[historyDepth] = selectedIndex;
   scrollHistory[historyDepth]    = scrollOffset;
   historyDepth++;
}

// restores the cursor/scroll position saved by the matching push, clamped
// to the now-current entryCount in case the directory shrank under us.
static void popNavHistory(void)
{
   if (historyDepth == 0) return;
   historyDepth--;
   selectedIndex = selectionHistory[historyDepth];
   scrollOffset  = scrollHistory[historyDepth];
   if (selectedIndex >= entryCount) selectedIndex = entryCount > 0 ? entryCount - 1 : 0;
   if (scrollOffset > selectedIndex) scrollOffset = selectedIndex;
}

static void loadDir(const char *path)
{
   cancelFolderSizer();  // any in-flight walker bails before we mutate entries
   sizerGeneration++;    // invalidate any stale results still in flight

   strCopy(currentPath, MAX_PATH_LEN, path);
   entryCount = 0;
   selectedIndex = 0;
   scrollOffset = 0;

   VfsDir dir;
   if (openDir(path, &dir) != 0) {
      labelsStale = 1;
      return;
   }

   // readDir skips "." / ".." (other dotfiles still show). at root the VFS yields
   // the enterable cellFs devices plus any mounted NTFS/exFAT volumes, so the old
   // root-only "unenterable" probe now lives inside the VFS, not here.
   char name[NAME_LEN];
   while (readDir(&dir, name, NAME_LEN, NULL) == 1) {
      char full[MAX_PATH_LEN];
      joinPath(full, MAX_PATH_LEN, path, name);

      if (!growArray(entries, &entryCapacity, entryCount + 1)) break;
      populateEntry(&entries[entryCount++], name, full);
   }
   closeDir(&dir);

   sortEntries();
   labelsStale = 1;

   if (breadcrumb) setBreadcrumbPath(breadcrumb, currentPath);
}

// the directory currently being browsed; used by the free-space widget to report
// the volume the user is in (and routed by the VFS to whatever backend owns it).
const char *getCurrentPath(void)
{
   return currentPath;
}

static void scrollToSelected(void);   // defined below; used by refreshMounts

// Handles a mount-set change (USB hotplug). Registered with the VFS as the
// mounts-changed callback (see initFileList), so pollMounts() drives it.
static void refreshMounts(void)
{
   // inside a folder whose volume was just pulled: drop straight back to root.
   if (!(currentPath[0] == '/' && currentPath[1] == '\0')) {
      VfsStat st;
      if (statPath(currentPath, &st) != 0) loadDir("/");
      return;
   }

   // at root: a volume appeared/disappeared. reload but keep the cursor on the same
   // row by name; if it's gone (the pulled volume), fall to the row above it, or the
   // new first row if it was already first.
   char prevName[NAME_LEN];
   prevName[0] = '\0';
   if (selectedIndex >= 0 && selectedIndex < entryCount)
      strCopy(prevName, NAME_LEN, entries[selectedIndex].name);
   int prevIndex = selectedIndex;

   loadDir("/");

   int found = 0;
   for (int i = 0; i < entryCount; i++)
      if (strEq(entries[i].name, prevName)) { selectedIndex = i; found = 1; break; }
   if (!found) selectedIndex = prevIndex > 0 ? prevIndex - 1 : 0;
   if (selectedIndex >= entryCount) selectedIndex = entryCount > 0 ? entryCount - 1 : 0;
   scrollToSelected();
}

// "drwxr-xr-x"-style text from st_mode permission bits: type char, then owner/group/other rwx triplets
static void formatPermissions(char *out, uint32_t mode, int isDir)
{
   out[0] = isDir ? 'd' : '-';
   for (int bit = 0; bit < 9; bit++)
      out[1 + bit] = (mode & (0400u >> bit)) ? "rwx"[bit % 3] : '-';
   out[10] = '\0';
}

static void rebuildLabels(void)
{
   // row labels (name, type, size, modified local time, permissions)
   for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
      int idx = scrollOffset + i;
      if (idx >= entryCount) {
         setLabelText(&labels[i], "");
         setLabelText(&sizeLabels[i], "");
         setLabelText(&typeLabels[i], "");
         setLabelText(&modifiedLabels[i], "");
         setLabelText(&permissionLabels[i], "");
         continue;
      }
      setLabelText(&labels[i],     entries[idx].name);
      setLabelText(&typeLabels[i], getFileTypeName(entries[idx].type));

      if (!entries[idx].sized) {
         setLabelText(&sizeLabels[i], EM_DASH);
      } else {
         char buf[24];
         formatSizeApprox(entries[idx].size, entries[idx].approx, buf);
         setLabelText(&sizeLabels[i], buf);
      }

      char modifiedText[20];
      formatDateTimeLocal(modifiedText, sizeof(modifiedText), entries[idx].modified);
      setLabelText(&modifiedLabels[i], modifiedText);

      char permissionText[12];
      formatPermissions(permissionText, entries[idx].mode, entries[idx].type == FILE_TYPE_FOLDER);
      setLabelText(&permissionLabels[i], permissionText);
   }

   // checked / total counter
   int checked = 0;
   for (int i = 0; i < entryCount; i++) {
      if (entries[i].checked) checked++;
   }
   char buf[32];
   snprintf(buf, sizeof(buf), "%d / %d", checked, entryCount);
   setLabelText(&counterLabel, buf);

   labelsStale = 0;
}

static void enterSelectedDir(void)
{
   if (selectedIndex < 0 || selectedIndex >= entryCount) return;
   if (entries[selectedIndex].type != FILE_TYPE_FOLDER) return;

   playAudioOnce(clickSfx);
   pushNavHistory();
   char next[MAX_PATH_LEN];
   joinPath(next, MAX_PATH_LEN, currentPath, entries[selectedIndex].name);
   loadDir(next);
}

// whether the selected row is an image the viewer can actually open.
static int selectedIsViewableImage(void)
{
   if (selectedIndex < 0 || selectedIndex >= entryCount) return 0;
   return entries[selectedIndex].type == FILE_TYPE_IMAGE &&
         isSupportedImageFormat(entries[selectedIndex].name);
}

// whether the selected row is an audio file the player can actually decode (wav/ogg).
static int selectedIsPlayableAudio(void)
{
   if (selectedIndex < 0 || selectedIndex >= entryCount) return 0;
   return entries[selectedIndex].type == FILE_TYPE_AUDIO &&
         isPlayableAudioFile(entries[selectedIndex].name);
}

// whether the selected row is a text file the editor can open.
static int selectedIsTextFile(void)
{
   if (selectedIndex < 0 || selectedIndex >= entryCount) return 0;
   return entries[selectedIndex].type == FILE_TYPE_TEXT;
}

// whether the selected row is a video file. any video opens the player, which probes the file and
// reports inside whether the PS3 can actually decode it (unlike image/audio, gated on decodability).
static int selectedIsVideo(void)
{
   if (selectedIndex < 0 || selectedIndex >= entryCount) return 0;
   return entries[selectedIndex].type == FILE_TYPE_VIDEO;
}

// cross handler: folders enter, supported images open in the viewer, playable audio opens in
// the player, text files open in the editor. anything else - and any image/audio/video the
// specific viewer can't decode - falls back to the hex viewer, so every non-folder row opens
// something.
static void activateSelectedEntry(void)
{
   if (selectedIndex < 0 || selectedIndex >= entryCount) return;

   if (entries[selectedIndex].type == FILE_TYPE_FOLDER) {
      enterSelectedDir();
      return;
   }

   char full[MAX_PATH_LEN];
   joinPath(full, MAX_PATH_LEN, currentPath, entries[selectedIndex].name);

   if (selectedIsViewableImage())      openImageViewer(full);
   else if (selectedIsPlayableAudio()) openAudioPlayer(full);
   else if (selectedIsVideo())         openVideoPlayer(full);
   else if (selectedIsTextFile())      openTextEditor(full);
   else                                openHexViewer(full);
}

static void onExitConfirmResolved(ConfirmChoice choice)
{
   if (choice == CONFIRM_CROSS) requestAppExit();
}

static void goToParentDir(void)
{
   if (strlen(currentPath) <= 1) {
      // already at root: Back means leaving the app, so make sure it's intentional
      askConfirm("Exit", "Are you sure you want to exit?", "Exit", NULL, "Cancel", onExitConfirmResolved);
      return;
   }

   playAudioOnce(clickSfx);
   toParentPath(currentPath);
   loadDir(currentPath);
   popNavHistory();
}

static void syncFooterButtons(void)
{
   int hasSelection = selectedIndex >= 0 && selectedIndex < entryCount;
   int isFolder      = hasSelection && entries[selectedIndex].type == FILE_TYPE_FOLDER;

   // every non-folder row opens something now (the hex viewer is the universal
   // fallback), so Cross is enabled whenever there's a selection at all.
   setFooterButtonEnabled(PAD_BTN_CROSS, hasSelection);

   // only retouch the label when the cross action actually changes, so we
   // don't re-rasterize the glyphs every frame.
   static int crossShowsOpen = -1;
   int showsOpen = hasSelection && !isFolder;
   if (showsOpen != crossShowsOpen) {
      setFooterButtonText(PAD_BTN_CROSS, showsOpen ? "Open" : "Enter");
      crossShowsOpen = showsOpen;
   }

   setFooterButtonEnabled(PAD_BTN_SQUARE, entryCount > 0);   // Circle stays enabled: at root it offers to exit the app
}

void initFileList(Font *font, GfxTexture spritesheet, Audio *click, Audio *check, int x, int y, int maxWidth, int rowHeight, int fontSize, uint32_t color, Breadcrumb *bc)
{
   breadcrumb = bc;
   clickSfx = click;
   checkSfx = check;
   listY = y;
   listRowHeight = rowHeight;
   entryCount = 0;
   entryCapacity = INITIAL_CAPACITY;
   entries = (FileEntry *)malloc(entryCapacity * sizeof(FileEntry));
   if (!entries) entryCapacity = 0;  // let the first growArray() allocate (and fail safely) instead of writing through NULL
   selectedIndex = 0;
   scrollOffset = 0;
   labelsStale = 1;
   historyDepth = 0;

   setMountsChangedCallback(refreshMounts);   // pollMounts() refreshes the root listing on USB hotplug

   initNineSlice(&hover, spritesheet, 47, y, 1882 - 47, rowHeight, spriteRegions[SPRITE_HIGHLIGHT], HIGHLIGHT_CAP, HIGHLIGHT_CAP);
   initLabel(&counterLabel, font, 55, 953, 200, AUTO, 20, color, TEXT_NOWRAP, NULL);
   for (int i = 0; i < COLUMN_HEADER_COUNT; i++)
      initLabel(&columnHeaderLabels[i], font, COLUMN_HEADERS[i].x, COLUMN_HEADER_Y, 240, AUTO, fontSize, color, TEXT_NOWRAP, COLUMN_HEADERS[i].text);
   addFooterButton(PAD_BTN_CROSS,  GLYPH_CROSS,  "Enter", activateSelectedEntry);
   addFooterButton(PAD_BTN_CIRCLE, GLYPH_CIRCLE, "Back",  goToParentDir);
   addFooterButton(PAD_BTN_SQUARE, GLYPH_SQUARE, "Mark",  NULL);

   for (int t = 0; t < FILE_TYPE_COUNT; t++)
      initImage(&fileIcons[t], spritesheet, 0, 0, 35, 43, spriteRegions[getFileTypeSprite(t)], GFX_FILTER_LINEAR);
   initImage(&usbIcon, spritesheet, 0, 0, spriteRegions[SPRITE_USB].w, spriteRegions[SPRITE_USB].h,
             spriteRegions[SPRITE_USB], GFX_FILTER_LINEAR);   // native size; centred on the folder at draw

   for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
      int ry = y + i * rowHeight;
      int cy = ry + (rowHeight - 25) / 2;

      initLabel(&labels[i], font, x, ry + 25, maxWidth, AUTO, fontSize, color, TEXT_NOWRAP_ELLIPSIS, NULL);
      initLabel(&typeLabels[i], font, COLUMN_TYPE_X, ry + 25, COLUMN_TYPE_W, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
      initLabel(&sizeLabels[i], font, COLUMN_SIZE_X, ry + 25, COLUMN_SIZE_W, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
      initLabel(&modifiedLabels[i], font, COLUMN_MODIFIED_X, ry + 25, COLUMN_MODIFIED_W, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
      initLabel(&permissionLabels[i], font, COLUMN_PERMISSIONS_X, ry + 25, COLUMN_PERMISSIONS_W, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
      initCheckbox(&checkboxes[i], spritesheet, 71, cy, 25, spriteRegions[SPRITE_CHECKBOX], spriteRegions[SPRITE_CHECKBOX_CHECKED]);
      initSlice(&separators[i], spritesheet, 47, ry - 1, 1884 - 47, 2, spriteRegions[SPRITE_SEPARATOR], 1);
   }

   loadDir("/");
   syncFooterButtons();
}

// square: tap toggles the focused row, hold toggles the whole directory.
// hold fires once at the threshold and suppresses the would-be tap on release.
#define SQUARE_HOLD_MS 400

static void setAllChecked(int value)
{
   for (int i = 0; i < entryCount; i++) entries[i].checked = value;
}

static int allEntriesChecked(void)
{
   for (int i = 0; i < entryCount; i++) if (!entries[i].checked) return 0;
   return entryCount > 0;
}

static void handleCheckInput(int hasSelection)
{
   static uint64_t pressedUs;
   static int      holdFired;

   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      pressedUs = sys_time_get_system_time();
      holdFired = 0;
      return;
   }

   if (isPadButtonHeld(PAD_BTN_SQUARE) && !holdFired && entryCount > 0 &&
       sys_time_get_system_time() - pressedUs >= SQUARE_HOLD_MS * 1000ULL) {
      setAllChecked(!allEntriesChecked());
      labelsStale = 1;
      holdFired = 1;
      playAudioOnce(checkSfx);
      return;
   }

   if (isPadButtonReleased(PAD_BTN_SQUARE) && !holdFired && hasSelection) {
      entries[selectedIndex].checked = !entries[selectedIndex].checked;
      labelsStale = 1;
      playAudioOnce(checkSfx);
   }
}

void updateFileList(void)
{
   if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_DOWN)) && entryCount > 0) {
      selectedIndex = (selectedIndex + 1) % entryCount;             // wraps bottom -> top
      scrollToSelected();
      playAudioOnce(clickSfx);
   }
   else if (isRepeatDue(&scrollRepeat, getPadButtonState(PAD_BTN_UP)) && entryCount > 0) {
      selectedIndex = (selectedIndex - 1 + entryCount) % entryCount;  // wraps top -> bottom
      scrollToSelected();
      playAudioOnce(clickSfx);
   }

   int hasSelection = selectedIndex >= 0 && selectedIndex < entryCount;

   handleCheckInput(hasSelection);

   if (labelsStale) rebuildLabels();
   syncFooterButtons();
   updateFolderSizer(&sizerCallbacks);
}

// non-zero if entry idx is currently on the clipboard (cut or copy).
static int entryIsMarked(int idx)
{
   char full[MAX_PATH_LEN];
   joinPath(full, MAX_PATH_LEN, currentPath, entries[idx].name);
   return isOnClipboard(full);
}

void drawFileList(void)
{
   for (int i = 0; i < COLUMN_HEADER_COUNT; i++) drawLabel(&columnHeaderLabels[i]);

   // hide top separator when the first visible row is selected (hover replaces it)
   if (entryCount == 0 || selectedIndex != scrollOffset) {
      drawSlice(&separators[0]);
   }

   for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
      int idx = scrollOffset + i;
      if (idx >= entryCount) break;

      // hide separators adjacent to the selected row (hover covers that area)
      if (i > 0 && idx != selectedIndex && idx - 1 != selectedIndex) {
         drawSlice(&separators[i]);
      }

      // draw hover background for selected row
      if (idx == selectedIndex) {
         moveNineSlice(&hover, 42, listY + i * listRowHeight);
         drawNineSlice(&hover);
      }

      // rows on the clipboard (cut or copy) render ghosted at 50% opacity
      int alpha = entryIsMarked(idx) ? MARKED_ROW_ALPHA : 0xFF;

      drawCheckboxAlpha(&checkboxes[i], entries[idx].checked, alpha);

      // draw type icon (and a centred USB badge for removable devices at root)
      int iconY = listY + i * listRowHeight + (listRowHeight - 43) / 2;
      moveImage(&fileIcons[entries[idx].type], 120, iconY);
      drawImageAlpha(&fileIcons[entries[idx].type], alpha);
      if (entries[idx].isUsb) {
         int usbW = spriteRegions[SPRITE_USB].w, usbH = spriteRegions[SPRITE_USB].h;
         moveImage(&usbIcon, 120 + (35 - usbW) / 2, iconY + (43 - usbH) / 2 + 4);
         drawImageAlpha(&usbIcon, alpha);
      }

      // draw labels
      drawLabelAlpha(&labels[i], alpha);
      drawLabelAlpha(&sizeLabels[i], alpha);
      drawLabelAlpha(&typeLabels[i], alpha);
      drawLabelAlpha(&modifiedLabels[i], alpha);
      drawLabelAlpha(&permissionLabels[i], alpha);
   }

   drawLabel(&counterLabel);
}

void termFileList(void)
{
   cancelFolderSizer();

   // release the text VRAM owned by every row label and the counter.
   for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
      freeLabel(&labels[i]);
      freeLabel(&sizeLabels[i]);
      freeLabel(&typeLabels[i]);
      freeLabel(&modifiedLabels[i]);
      freeLabel(&permissionLabels[i]);
   }
   freeLabel(&counterLabel);
   for (int i = 0; i < COLUMN_HEADER_COUNT; i++) freeLabel(&columnHeaderLabels[i]);

   free(entries);
   entries = NULL;
   entryCount = 0;
   entryCapacity = 0;
   freeDelete();
   freeClipboard();
   freeZip();
}

static SelectionSummary summary;

// counts checked rows; if lastChecked is non-null, stores the last checked
// row index (or -1 when none are checked).
static int countChecked(int *lastChecked)
{
   int count = 0, last = -1;
   for (int i = 0; i < entryCount; i++) {
      if (entries[i].checked) { count++; last = i; }
   }
   if (lastChecked) *lastChecked = last;
   return count;
}

const SelectionSummary *getSelectionSummary(void)
{
   int lastChecked;
   int checkedCount = countChecked(&lastChecked);

   static char title[NAME_LEN];
   static char subtitle[32];
   static char detail[48];

   // multi-check: aggregate "N items", total file count, total bytes
   if (checkedCount > 1) {
      int      totalFiles = 0;
      uint64_t totalBytes = 0;
      int      anyApprox  = 0;
      for (int i = 0; i < entryCount; i++) {
         if (!entries[i].checked) continue;
         if (!entries[i].sized || entries[i].approx) anyApprox = 1;
         totalFiles += entries[i].fileCount;
         totalBytes += entries[i].size;
      }
      const char *plus = anyApprox ? "+" : "";
      char sizeBuf[24];
      formatSizeApprox(totalBytes, anyApprox, sizeBuf);
      snprintf(title,    sizeof(title),    "%d items",    checkedCount);
      snprintf(subtitle, sizeof(subtitle), "%d files%s", totalFiles, plus);
      strCopy(detail, sizeof(detail), sizeBuf);
      summary.title    = title;
      summary.subtitle = subtitle;
      summary.detail   = detail;
      summary.icon     = spriteRegions[SPRITE_GENERIC_MULTI];
      return &summary;
   }

   // single target: the one checked row, else the cursor row
   int idx = (checkedCount == 1) ? lastChecked : selectedIndex;
   if (idx < 0 || idx >= entryCount) {
      summary.title    = "";
      summary.subtitle = "";
      summary.detail   = "";
      summary.icon     = spriteRegions[SPRITE_GENERIC];
      return &summary;
   }

   // detail line: "—" while unsized, "N files, M.M MB" for folders, plain size for files
   const FileEntry *e = &entries[idx];
   if (!e->sized) {
      strCopy(detail, sizeof(detail), EM_DASH);
   } else if (e->type == FILE_TYPE_FOLDER) {
      char sizeBuf[24];
      formatSizeApprox(e->size, e->approx, sizeBuf);
      const char *plus = e->approx ? "+" : "";
      snprintf(detail, sizeof(detail), "%d files%s, %s", e->fileCount, plus, sizeBuf);
   } else {
      formatSize(e->size, detail);
   }
   summary.title    = e->name;
   summary.subtitle = getFileTypeName(e->type);
   summary.detail   = detail;
   summary.icon     = spriteRegions[getFileTypeSprite(e->type)];
   return &summary;
}

// true when name's extension is ".zip" (case-insensitive).
static int hasZipExtension(const char *name)
{
   const char *ext = getExtension(name);
   return ext && strCmpICase(ext, "zip") == 0;
}

// true when the active row is the entire target - nothing else checked, and if the
// active row itself is checked, that's fine too - and that row is a .zip file. unzip
// operates on entries[selectedIndex] alone (see unzipActive), so this is also exactly
// "unzip is offered": a checked row other than the active one disqualifies it, same as
// two or more checked rows would.
static int targetIsSingleZipFile(void)
{
   if (entryCount == 0) return 0;
   int checkedCount = countChecked(NULL);
   if (checkedCount > 1) return 0;
   if (checkedCount == 1 && !entries[selectedIndex].checked) return 0;
   if (entries[selectedIndex].type == FILE_TYPE_FOLDER) return 0;
   return hasZipExtension(entries[selectedIndex].name);
}

// cut, copy and delete apply to any non-empty selection (one row or many).
// paste is offered whenever the clipboard holds something - even in an empty
// directory, since pasting there is valid. rename follows delete and acts on the
// active row alone (ignoring checkboxes), so it is offered whenever the directory
// is non-empty. zip and unzip are opposites of the same single-zip-target check:
// unzip needs exactly that one target and it to be a zip (zipping a lone zip is
// pointless, and unzip only ever acts on the active row - see targetIsSingleZipFile);
// zip is offered in every other non-empty case. order is cut, copy, paste, delete,
// rename, zip, unzip.
const SelectionAction *getAvailableActions(int *outCount)
{
   static SelectionAction list[9];
   int n = 0;
   if (entryCount > 0)     { list[n++] = ACTION_CUT; list[n++] = ACTION_COPY; }
   if (!isClipboardEmpty())  list[n++] = ACTION_PASTE;
   if (entryCount > 0)     { list[n++] = ACTION_DELETE; list[n++] = ACTION_RENAME; }
   if (entryCount > 0 && !targetIsSingleZipFile()) list[n++] = ACTION_ZIP;
   if (targetIsSingleZipFile())                    list[n++] = ACTION_UNZIP;
   // creation always applies to the current directory, even when it is empty.
   list[n++] = ACTION_NEW_FILE;
   list[n++] = ACTION_NEW_FOLDER;
   *outCount = n;
   return list;
}

static void scrollToSelected(void);  // defined below; used by the create/rename helpers

// name of the highlighted row, independent of any checkbox marks; NULL in an
// empty directory. rename acts on this row alone, ignoring checked items.
const char *getActiveEntryName(void)
{
   if (entryCount == 0) return NULL;
   return entries[selectedIndex].name;
}

// keeps the active row on screen after the index is moved programmatically.
// flags labels stale when the visible window actually shifts.
static void scrollToSelected(void)
{
   int previousOffset = scrollOffset;
   if (selectedIndex < scrollOffset)
      scrollOffset = selectedIndex;
   else if (selectedIndex >= scrollOffset + FILE_LIST_PAGE_SIZE)
      scrollOffset = selectedIndex - FILE_LIST_PAGE_SIZE + 1;
   if (scrollOffset != previousOffset) labelsStale = 1;
}

// moves the cursor to the existing row named name (if present) and scrolls it
// into view; no filesystem change. used when a create/rename lands on something
// that must not be replaced, so the cursor still ends up on the conflicting item.
static void selectEntryByName(const char *name)
{
   for (int i = 0; i < entryCount; i++)
      if (strEq(entries[i].name, name)) { selectedIndex = i; break; }
   scrollToSelected();
   labelsStale = 1;
}

// creates an empty file or a folder named name in the current directory. a file
// of the same name is truncated (writeFile uses O_CREAT | O_TRUNC); makeDir is a
// no-op on an existing folder. the listing is reloaded from disk - which resets
// the cursor - so the new item is found by name and the cursor parked on it.
static int createNewEntry(const char *name, int isFolder)
{
   if (!isValidFileName(name)) return -1;

   char path[MAX_PATH_LEN];
   joinPath(path, MAX_PATH_LEN, currentPath, name);

   int rc = isFolder ? makeDir(path) : writeFile(path, "", 0);
   if (rc != 0) return -1;

   loadDir(currentPath);          // resets selectedIndex / scrollOffset to 0
   selectEntryByName(name);       // park the cursor on the new item
   return 0;
}

// the name a pending create/rename dialog is resolving. one dialog is up at a
// time (the overlay is modal), so a single buffer is enough. sized to a full path
// since isValidFileName admits names up to MAX_PATH_LEN.
static char pendingName[MAX_PATH_LEN];

// the active row's name and scroll offset just before a zip/unzip task launches, so a
// cancelled or failed run can restore the exact prior view instead of landing on row 0
// (pendingName's target never got created, so selectEntryByName(pendingName) fails) - and
// selectEntryByName's own scrollToSelected only guarantees the row is visible, not that the
// scroll offset matches what it was before, so the offset is restored separately.
static char preTaskName[NAME_LEN];
static int  preTaskScrollOffset;

static char mergeBuf[64 * 1024];   // payload scratch for a synchronous rename-merge

// how a merge resolves a file-leaf collision; also what the cross/circle answer
// of a merge prompt maps to.
enum { KEEP_EXISTING = 0, REPLACE_EXISTING = 1 };

// the prompt only distinguishes no / one / many collisions, so a scan can stop
// as soon as it reaches this count - two or more all read as "many".
#define MANY_CONFLICTS 2

// cross means replace the existing file; anything else (circle) means keep it.
static int replaceChosen(ConfirmChoice choice)
{
   return choice == CONFIRM_CROSS ? REPLACE_EXISTING : KEEP_EXISTING;
}

// asks how to resolve file collisions for a merge and hands the answer to
// onResult: nothing to ask when none collide (resolves immediately, replace being
// moot), a single Replace/Keep for one, Replace All/Keep All for several. shared
// by paste and rename so the wording lives in one place.
static void promptMergeConflicts(int conflicts, ConfirmCallback onResult)
{
   if (conflicts <= 0)
      onResult(CONFIRM_CROSS);
   else if (conflicts == 1)
      askConfirm("Replace File", "A file already exists. Replace it?",
                 "Replace", NULL, "Keep", onResult);
   else
      askConfirm("Replace Files", "Some files already exist. Replace them?",
                 "Replace All", NULL, "Keep All", onResult);
}

// plain same-directory rename of the active row to newName (no collision: the
// caller has cleared the destination or knows it is free). same-volume, so the
// bare lv2 rename suffices. refreshes and parks the cursor on the result.
static void renamePlain(const char *newName)
{
   char oldPath[MAX_PATH_LEN], newPath[MAX_PATH_LEN];
   joinPath(oldPath, MAX_PATH_LEN, currentPath, entries[selectedIndex].name);
   joinPath(newPath, MAX_PATH_LEN, currentPath, newName);
   if (renamePath(oldPath, newPath) != 0) return;
   loadDir(currentPath);
   selectEntryByName(newName);
}

// wipe whatever sits at newName, then rename onto it. this is the only rename path
// that deletes a populated target, and only ever on an explicit Replace.
static void renameReplace(const char *newName)
{
   char newPath[MAX_PATH_LEN];
   joinPath(newPath, MAX_PATH_LEN, currentPath, newName);
   deleteTree(newPath, NULL);
   renamePlain(newName);
}

// fold the active folder's contents into the existing folder newName (replacing
// or keeping colliding files per replaceConflicts), then drop the now redundant
// source folder. runs synchronously - fine for this rare action, though a very
// large merge briefly blocks the UI (no progress bar).
static void renameMerge(const char *newName, int replaceConflicts)
{
   char oldPath[MAX_PATH_LEN], newPath[MAX_PATH_LEN];
   joinPath(oldPath, MAX_PATH_LEN, currentPath, entries[selectedIndex].name);
   joinPath(newPath, MAX_PATH_LEN, currentPath, newName);
   if (mergeTreeProgress(oldPath, newPath, replaceConflicts, mergeBuf, sizeof mergeBuf, NULL, NULL) == 0)
      deleteTree(oldPath, NULL);
   loadDir(currentPath);
   selectEntryByName(newName);
}

// New File: the field collided with an existing file and the user chose. Cancel
// does nothing at all - the cursor stays where it was.
static void onReplaceFileConfirmed(ConfirmChoice choice)
{
   if (choice == CONFIRM_CROSS) createNewEntry(pendingName, 0);  // writeFile truncates the old file
}

// New File create. validates, then: free name -> create; collides with a folder
// -> never replaced, just select it; collides with a file -> Replace / Cancel.
void createFile(const char *name)
{
   if (!isValidFileName(name)) return;

   char path[MAX_PATH_LEN];
   joinPath(path, MAX_PATH_LEN, currentPath, name);

   if (!fileExists(path)) { createNewEntry(name, 0); return; }
   if (isDir(path))       { selectEntryByName(name); return; }  // can't replace a folder with a file

   strCopy(pendingName, sizeof pendingName, name);
   char msg[MAX_PATH_LEN + 32];
   snprintf(msg, sizeof msg, "\"%s\" already exists. Replace it?", name);
   askConfirm("Replace File", msg, "Replace", NULL, "Cancel", onReplaceFileConfirmed);
}

// New Folder create. validates, then: free name -> create the folder; name taken
// (folder or file) -> merge route, which for an empty new folder is a no-op, so
// just select the existing item. never deletes anything.
void createFolder(const char *name)
{
   if (!isValidFileName(name)) return;

   char path[MAX_PATH_LEN];
   joinPath(path, MAX_PATH_LEN, currentPath, name);

   if (!fileExists(path)) createNewEntry(name, 1);
   else                   selectEntryByName(name);
}

// inner step of a rename-merge: replace or keep the colliding files, then merge.
static void onRenameMergeResolved(ConfirmChoice choice)
{
   renameMerge(pendingName, replaceChosen(choice));
}

// the user picked Merge / Replace / Cancel for a rename onto an existing item.
static void onRenameCollision(ConfirmChoice choice)
{
   if (choice == CONFIRM_CIRCLE) return;  // Cancel: do nothing, leave the cursor put

   char oldPath[MAX_PATH_LEN], newPath[MAX_PATH_LEN];
   joinPath(oldPath, MAX_PATH_LEN, currentPath, entries[selectedIndex].name);
   joinPath(newPath, MAX_PATH_LEN, currentPath, pendingName);

   // Replace, or Merge with a non-folder on either side (merging files is
   // meaningless), both collapse to a clean replace of the destination.
   if (choice == CONFIRM_SQUARE || !(isDir(oldPath) && isDir(newPath)))
      renameReplace(pendingName);
   else
      promptMergeConflicts(countTreeConflicts(oldPath, newPath, MANY_CONFLICTS), onRenameMergeResolved);
}

// renames the highlighted row to newName within the current directory (ignoring
// checkboxes). an invalid name or a rename to the same name is a no-op. a free
// destination renames immediately; an existing one opens a Merge / Replace /
// Cancel prompt (Merge folds folders together, Replace clobbers, Cancel aborts).
void renameActiveTo(const char *newName)
{
   if (entryCount == 0) return;
   if (!isValidFileName(newName)) return;
   if (strEq(entries[selectedIndex].name, newName)) return;  // unchanged

   char newPath[MAX_PATH_LEN];
   joinPath(newPath, MAX_PATH_LEN, currentPath, newName);
   if (!fileExists(newPath)) { renamePlain(newName); return; }

   strCopy(pendingName, sizeof pendingName, newName);
   char msg[MAX_PATH_LEN + 32];
   snprintf(msg, sizeof msg, "\"%s\" already exists.", newName);
   askConfirm("Rename", msg, "Merge", "Replace", "Cancel", onRenameCollision);
}

int getSelectionCount(void)
{
   int checked = countChecked(NULL);
   if (checked > 0) return checked;
   return entryCount > 0 ? 1 : 0;  // the active row, if any
}

// whether row i is part of the current action target: the checked rows, or the
// active row when nothing is checked. checkedCount comes from countChecked().
static int entryTargeted(int i, int checkedCount)
{
   return checkedCount > 0 ? entries[i].checked : (i == selectedIndex);
}

// an entry's size is exact for files, and for folders only once the sizer has
// fully walked them (sized and not budget-truncated).
static int entrySizeIsExact(const FileEntry *e) { return e->sized && !e->approx; }

// main-thread finisher: refresh the listing and reposition the cursor.
static void onDeleteFinished(int cancelled)
{
   (void)cancelled;
   clearClipboard();      // a delete invalidates any pending cut/copy
   loadDir(currentPath);  // resets selectedIndex/scrollOffset to 0

   // row above the topmost deleted item; if that was the top of the list,
   // index 0 is now the row that sat just below it.
   selectedIndex = delTopmost > 0 ? delTopmost - 1 : 0;
   scrollToSelected();
   labelsStale = 1;
}

void deleteSelection(void)
{
   if (entryCount == 0) return;

   int checkedCount = countChecked(NULL);

   beginDelete();
   delTopmost = -1;
   int gathered = 0;
   char full[MAX_PATH_LEN];
   for (int i = 0; i < entryCount; i++) {
      if (!entryTargeted(i, checkedCount)) continue;
      if (delTopmost < 0) delTopmost = i;
      joinPath(full, MAX_PATH_LEN, currentPath, entries[i].name);
      if (addToDelete(full, entries[i].size, entrySizeIsExact(&entries[i])) != 0) break;
      gathered++;
   }
   if (gathered == 0) return;

   startProgress("Deleting...", "Please wait while the selected items are deleted.",
                 runDelete, onDeleteFinished);
}

// gathers the current selection (checked rows, or the active row when nothing
// is checked) onto the clipboard in the given mode.
static void clipboardFromSelection(ClipboardMode mode)
{
   if (entryCount == 0) return;
   int checkedCount = countChecked(NULL);

   beginClipboard(mode);
   char full[MAX_PATH_LEN];
   for (int i = 0; i < entryCount; i++) {
      if (!entryTargeted(i, checkedCount)) continue;
      joinPath(full, MAX_PATH_LEN, currentPath, entries[i].name);
      addToClipboard(full, entries[i].size, entrySizeIsExact(&entries[i]));
   }
}

void cutSelection(void)  { clipboardFromSelection(CLIP_CUT); }
void copySelection(void) { clipboardFromSelection(CLIP_COPY); }

// a completed paste consumes the clipboard; a cancelled one keeps it for retry.
static void onPasteFinished(int cancelled)
{
   loadDir(currentPath);

   // land the cursor on the topmost pasted item: the first row (in sort order)
   // whose name matches a pasted source's base name. a move or a copy into
   // another folder lands the destination under that name; a same-folder copy is
   // suffixed, so this finds the original it sits beside. read the clipboard
   // before it is cleared below.
   int clipCount = getClipboardCount();
   for (int i = 0; i < entryCount; i++) {
      int hit = 0;
      for (int c = 0; c < clipCount && !hit; c++)
         hit = strEq(entries[i].name, getBaseName(getClipboardPath(c)));
      if (hit) { selectedIndex = i; break; }
   }
   scrollToSelected();

   if (!cancelled) clearClipboard();
   labelsStale = 1;

   // surface an interrupted transfer (e.g. USB pulled mid-copy). partial output is
   // left in place on purpose; a single-OK alert just tells the user what happened.
   if (pasteHadError())
      askConfirm("Device Disconnected", "Some items may be incomplete.",
                 "OK", NULL, NULL, NULL);
}

// launches the paste worker with the conflict policy already chosen.
static void startPasteRun(int replaceExisting)
{
   setPasteReplaceOnConflict(replaceExisting);
   int moving = (getClipboardMode() == CLIP_CUT);
   startProgress(moving ? "Moving..." : "Copying...",
                 moving ? "Please wait while the selected items are moved."
                        : "Please wait while the selected items are copied.",
                 runPaste, onPasteFinished);
}

// the Replace/Keep (or Replace All/Keep All) answer for a conflicting paste.
static void onPasteConflictResolved(ConfirmChoice choice) { startPasteRun(replaceChosen(choice)); }

void pasteClipboard(void)
{
   if (isClipboardEmpty()) return;
   setPasteDest(currentPath);  // countClipboardConflicts and runPaste both read this

   // ask how to resolve collisions up front (and only when they exist) via the
   // shared prompt, then run the paste with the chosen policy.
   promptMergeConflicts(countClipboardConflicts(MANY_CONFLICTS), onPasteConflictResolved);
}

// a completed zip parks the cursor on the new archive; a cancelled or failed run
// restores the cursor to wherever it was before the task launched (partial output,
// if any, is left in place - matches the paste/delete convention).
static void onZipFinished(int cancelled)
{
   loadDir(currentPath);
   if (!cancelled && !zipHadError()) {
      selectEntryByName(pendingName);
   } else {
      // seed scrollOffset with the pre-task value so selectEntryByName's own
      // scrollToSelected() keeps it as-is when the row is still in view, and only
      // adjusts (in whichever direction) when the reload actually moved it out of view -
      // overwriting scrollToSelected's result afterward would drop its bottom-edge clamp.
      scrollOffset = preTaskScrollOffset;
      selectEntryByName(preTaskName);
   }
   if (!cancelled && zipHadError())
      askConfirm("Zip Failed", "The archive could not be created.", "OK", NULL, NULL, NULL);
}

static void startZipRun(void)
{
   startProgress("Zipping...", "Please wait while the selected items are compressed.", runZip, onZipFinished);
}

// the Replace/Cancel answer for a zip name colliding with an existing file.
static void onZipReplaceConfirmed(ConfirmChoice choice)
{
   if (choice == CONFIRM_CROSS) startZipRun();
}

// zips the current selection (checked rows, or the active row when none are
// checked) into name within the current directory. a free name zips at once; a
// collision with an existing file opens a Replace / Cancel prompt (mirrors
// createFile); a collision with a folder is refused outright, same as createFile.
void zipSelectionTo(const char *name)
{
   if (entryCount == 0) return;
   if (!isValidFileName(name)) return;

   char destPath[MAX_PATH_LEN];
   if (!joinPath(destPath, MAX_PATH_LEN, currentPath, name)) return;
   if (isDir(destPath)) return;

   int checkedCount = countChecked(NULL);
   char full[MAX_PATH_LEN];

   // refuse outright if the archive would overwrite one of the very items being
   // zipped: zipPathsProgress truncates destPath before reading its sources, so a
   // name collision here would corrupt that source out from under itself. compared
   // case-insensitively since exFAT/NTFS treat differently-cased paths as the same file.
   for (int i = 0; i < entryCount; i++) {
      if (!entryTargeted(i, checkedCount)) continue;
      if (!joinPath(full, MAX_PATH_LEN, currentPath, entries[i].name)) return;
      if (strCmpICase(full, destPath) == 0) {
         askConfirm("Cannot Zip", "That name matches one of the items being zipped. Choose a different name.",
                    "OK", NULL, NULL, NULL);
         return;
      }
   }

   strCopy(pendingName, sizeof pendingName, name);
   strCopy(preTaskName, sizeof preTaskName, entries[selectedIndex].name);
   preTaskScrollOffset = scrollOffset;
   setZipDest(destPath);

   beginZip();
   for (int i = 0; i < entryCount; i++) {
      if (!entryTargeted(i, checkedCount)) continue;
      if (!joinPath(full, MAX_PATH_LEN, currentPath, entries[i].name)) break;
      if (addToZip(full) != 0) break;
   }

   if (!fileExists(destPath)) { startZipRun(); return; }

   char msg[MAX_PATH_LEN + 32];
   snprintf(msg, sizeof msg, "\"%s\" already exists. Replace it?", name);
   askConfirm("Replace File", msg, "Replace", NULL, "Cancel", onZipReplaceConfirmed);
}

// a completed unzip parks the cursor on the extracted subfolder; a cancelled or
// failed run restores the cursor to wherever it was before the task launched
// (partial output, if any, is left in place).
static void onUnzipFinished(int cancelled)
{
   loadDir(currentPath);
   if (!cancelled && !unzipHadError()) {
      selectEntryByName(pendingName);
   } else {
      // see onZipFinished for why scrollOffset is seeded before, not overwritten after.
      scrollOffset = preTaskScrollOffset;
      selectEntryByName(preTaskName);
   }
   if (!cancelled && unzipHadError())
      askConfirm("Extraction Failed", "The archive could not be fully extracted.", "OK", NULL, NULL, NULL);
}

static void startUnzipRun(void)
{
   startProgress("Extracting...", "Please wait while the archive is extracted.", runUnzip, onUnzipFinished);
}

// the Replace All/Keep All answer for extracting on top of an existing subfolder.
static void onUnzipConflictResolved(ConfirmChoice choice)
{
   setUnzipReplaceOnConflict(replaceChosen(choice));
   startUnzipRun();
}

// extracts the active row (a .zip file, per getAvailableActions's gating) into a
// new subfolder named after the archive. a free name extracts at once; an
// existing subfolder merges in, with a Replace All / Keep All prompt for file
// collisions inside it.
void unzipActive(void)
{
   if (entryCount == 0) return;

   char zipPath[MAX_PATH_LEN], destDir[MAX_PATH_LEN], stem[NAME_LEN];
   if (!joinPath(zipPath, MAX_PATH_LEN, currentPath, entries[selectedIndex].name)) return;

   // strip the ".zip" extension (4 chars); relies on getAvailableActions's hasZipExtension
   // gate having already confirmed the active row ends in ".zip".
   strCopy(stem, sizeof stem, entries[selectedIndex].name);
   int dot = getStrLen(stem) - 4;
   if (dot > 0) stem[dot] = '\0';

   if (!joinPath(destDir, MAX_PATH_LEN, currentPath, stem)) return;
   strCopy(pendingName, sizeof pendingName, stem);
   strCopy(preTaskName, sizeof preTaskName, entries[selectedIndex].name);
   preTaskScrollOffset = scrollOffset;

   setUnzipSource(zipPath);
   setUnzipDest(destDir);

   if (!fileExists(destDir)) { setUnzipReplaceOnConflict(REPLACE_EXISTING); startUnzipRun(); return; }
   if (!isDir(destDir)) return;   // a file sits where the subfolder would go: refuse, same as createFile

   // unlike promptMergeConflicts, this can't first count actual collisions - doing so would
   // mean opening and walking the archive's central directory a second time just to ask the
   // question. so the wording stays generic ("its files") rather than the 0/1/many phrasing
   // rename/paste use, even though the folder existing doesn't guarantee any file collides.
   askConfirm("Extracting Archive", "A folder with this name already exists. Replace its files?",
              "Replace All", NULL, "Keep All", onUnzipConflictResolved);
}
