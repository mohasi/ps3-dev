// file-list - scrollable directory listing widget
#include "widgets/file-list.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/slice.h"
#include "ui/breadcrumb.h"
#include "sprite-regions.h"
#include "folder-sizer.h"
#include "file-type.h"
#include "file.h"
#include "pad.h"
#include "audio.h"
#include "button-repeat.h"
#include "string-utilities.h"
#include <string.h>
#include <stdlib.h>
#include <sys/sys_time.h>

#define NAME_LEN           256
#define INITIAL_CAPACITY   256
#define HISTORY_MAX        16

typedef struct {
    char name[NAME_LEN];
    uint64_t size;
    int fileCount;    // 1 for files; recursive count for folders (valid when sized)
    FileType type;
    int checked;
    int sized;        // folders: 0 until folder-sizer visits it
    int approx;       // sized but the walker hit its budget; size/fileCount are a lower bound
} FileEntry;

static FileEntry *entries;
static int entryCount;
static int entryCapacity;
static int selectedIndex;
static int scrollOffset;

static Label labels[FILE_LIST_PAGE_SIZE];
static Label sizeLabels[FILE_LIST_PAGE_SIZE];
static Label typeLabels[FILE_LIST_PAGE_SIZE];
static Label counterLabel;
static int listY, listRowHeight;
static int labelsStale;
static char currentPath[MAX_PATH_LEN];
static int selectionHistory[HISTORY_MAX];
static int scrollHistory[HISTORY_MAX];
static int historyDepth;
static ButtonRepeat scrollRepeat;

static Image checkboxes[FILE_LIST_PAGE_SIZE];
static Image checkedBoxes[FILE_LIST_PAGE_SIZE];
static Image fileIcons[FILE_TYPE_COUNT];
static Slice separators[FILE_LIST_PAGE_SIZE];
static Slice hover;
static Breadcrumb *breadcrumb;
static Audio *clickSfx;
static Audio *checkSfx;

// folder-sizer source: lets the background walker visit unsized folders in
// this directory and write results back into the matching entry.
static int sizerCount(void) { return entryCount; }

static int sizerNeedsSizing(int i, char *outPath, int cap)
{
    if (i < 0 || i >= entryCount) return 0;
    if (entries[i].sized) return 0;
    if (entries[i].type != FILE_TYPE_FOLDER) return 0;
    joinPath(outPath, cap, currentPath, entries[i].name);
    return 1;
}

static void sizerApplyResult(int i, uint64_t bytes, int files, int approx)
{
    if (i < 0 || i >= entryCount) return;
    entries[i].size      = bytes;
    entries[i].fileCount = files;
    entries[i].approx    = approx;
    entries[i].sized     = 1;
    labelsStale          = 1;
}

static const FolderSizerSource sizerSource = { sizerCount, sizerNeedsSizing, sizerApplyResult };

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

// fills entry from a single stat() of the full path. directories defer
// their size to the background folder-sizer (sized=0); files are complete
// up front. unreadable entries are kept as a generic 0-byte placeholder
// so the row still shows up rather than being silently dropped.
static void populateEntry(FileEntry *e, const char *name, const char *fullPath)
{
    strCopy(e->name, NAME_LEN, name);
    e->checked   = 0;
    e->fileCount = 0;
    e->approx    = 0;

    CellFsStat st;
    if (cellFsStat(fullPath, &st) != CELL_FS_SUCCEEDED) {
        e->type      = FILE_TYPE_GENERIC;
        e->size      = 0;
        e->fileCount = 1;
        e->sized     = 1;
        return;
    }

    int isDir = (st.st_mode & CELL_FS_S_IFDIR) != 0;
    e->type = classifyFileType(name, isDir);
    if (isDir) {
        e->size  = 0;
        e->sized = 0;  // folder-sizer fills this in
    } else {
        e->size      = st.st_size;
        e->fileCount = 1;
        e->sized     = 1;
    }
}

// remembers the cursor/scroll position of the current directory so circle
// can restore it when popping back. silently drops oldest entries past
// HISTORY_MAX -- we just lose precise scroll memory beyond that depth.
static void pushNavHistory(void)
{
    if (historyDepth >= HISTORY_MAX) return;
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

    strCopy(currentPath, MAX_PATH_LEN, path);
    entryCount = 0;
    selectedIndex = 0;
    scrollOffset = 0;

    int fd;
    if (cellFsOpendir(path, &fd) != CELL_FS_SUCCEEDED) {
        labelsStale = 1;
        return;
    }

    // root contains devkit / system mounts (app_home, dev_flash2/3, host_root)
    // that userland cannot actually enter. probe each so they don't clutter the
    // listing. only worth doing at root; below root cellFsStat already filters.
    int filterUnenterable = (path[0] == '/' && path[1] == '\0');

    CellFsDirent ent;
    uint64_t readBytes;
    while (cellFsReaddir(fd, &ent, &readBytes) == CELL_FS_SUCCEEDED && readBytes > 0) {
        if (ent.d_name[0] == '.') continue;

        char full[MAX_PATH_LEN];
        joinPath(full, MAX_PATH_LEN, path, ent.d_name);

        if (filterUnenterable) {
            int probe;
            if (cellFsOpendir(full, &probe) != CELL_FS_SUCCEEDED) continue;
            cellFsClosedir(probe);
        }

        if (entryCount >= entryCapacity) {
            int newCap = entryCapacity * 2;
            FileEntry *grown = (FileEntry *)realloc(entries, newCap * sizeof(FileEntry));
            if (!grown) break;
            entries = grown;
            entryCapacity = newCap;
        }

        populateEntry(&entries[entryCount++], ent.d_name, full);
    }
    cellFsClosedir(fd);

    sortEntries();
    labelsStale = 1;

    if (breadcrumb) setBreadcrumbPath(breadcrumb, currentPath);
}

// formats bytes as a human size with a trailing '+' if the value is only a
// lower bound (folder walker hit its time budget). buf must hold >= 16.
static void formatSizeApprox(uint64_t bytes, int approx, char *buf)
{
    formatSize(bytes, buf);
    if (!approx) return;
    int n = strlen(buf);
    buf[n]     = '+';
    buf[n + 1] = '\0';
}

static void rebuildLabels(void)
{
    // row labels (name, type, size)
    for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
        int idx = scrollOffset + i;
        if (idx >= entryCount) {
            setLabelText(&labels[i], "");
            setLabelText(&sizeLabels[i], "");
            setLabelText(&typeLabels[i], "");
            continue;
        }
        setLabelText(&labels[i],     entries[idx].name);
        setLabelText(&typeLabels[i], fileTypeName(entries[idx].type));

        if (!entries[idx].sized) {
            setLabelText(&sizeLabels[i], EM_DASH);
        } else {
            char buf[16];
            formatSizeApprox(entries[idx].size, entries[idx].approx, buf);
            setLabelText(&sizeLabels[i], buf);
        }
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
    selectedIndex = 0;
    scrollOffset = 0;
    labelsStale = 1;
    historyDepth = 0;

    initSlice(&hover, spritesheet, 47, y, 1882 - 47, rowHeight, spriteRegions[SPRITE_HOVER], 8);
    initLabel(&counterLabel, font, 55, 953, 200, AUTO, 20, color, TEXT_NOWRAP, NULL);

    for (int t = 0; t < FILE_TYPE_COUNT; t++)
        initImage(&fileIcons[t], spritesheet, 0, 0, 35, 43, spriteRegions[fileTypeSprite(t)], GFX_FILTER_LINEAR);

    for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
        int ry = y + i * rowHeight;
        int cy = ry + (rowHeight - 25) / 2;

        initLabel(&labels[i], font, x, ry + 25, maxWidth, AUTO, fontSize, color, TEXT_NOWRAP_ELLIPSIS, NULL);
        initLabel(&sizeLabels[i], font, 1467, ry + 25, 200, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
        initLabel(&typeLabels[i], font, 1722, ry + 25, 150, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
        initImage(&checkboxes[i], spritesheet, 71, cy, 25, 25, spriteRegions[SPRITE_CHECKBOX], GFX_FILTER_LINEAR);
        initImage(&checkedBoxes[i], spritesheet, 71, cy, 25, 25, spriteRegions[SPRITE_CHECKBOX_CHECKED], GFX_FILTER_LINEAR);
        initSlice(&separators[i], spritesheet, 47, ry - 1, 1884 - 47, 2, spriteRegions[SPRITE_SEPARATOR], 1);
    }

    loadDir("/");
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

    if (pad.btn.square == BTN_PRESSED) {
        pressedUs = sys_time_get_system_time();
        holdFired = 0;
        return;
    }

    if (pad.btn.square == BTN_HELD && !holdFired && entryCount > 0 &&
        sys_time_get_system_time() - pressedUs >= SQUARE_HOLD_MS * 1000ULL) {
        setAllChecked(!allEntriesChecked());
        labelsStale = 1;
        holdFired = 1;
        playSfxOnce(checkSfx);
        return;
    }

    if (pad.btn.square == BTN_RELEASED && !holdFired && hasSelection) {
        entries[selectedIndex].checked = !entries[selectedIndex].checked;
        labelsStale = 1;
        playSfxOnce(checkSfx);
    }
}

void updateFileList(void)
{
    if (buttonRepeated(&scrollRepeat, pad.btn.down) && selectedIndex < entryCount - 1) {
        selectedIndex++;
        if (selectedIndex >= scrollOffset + FILE_LIST_PAGE_SIZE) {
            scrollOffset = selectedIndex - FILE_LIST_PAGE_SIZE + 1;
            labelsStale = 1;
        }
        playSfxOnce(clickSfx);
    }
    else if (buttonRepeated(&scrollRepeat, pad.btn.up) && selectedIndex > 0) {
        selectedIndex--;
        if (selectedIndex < scrollOffset) {
            scrollOffset = selectedIndex;
            labelsStale = 1;
        }
        playSfxOnce(clickSfx);
    }

    int hasSelection = selectedIndex >= 0 && selectedIndex < entryCount;

    handleCheckInput(hasSelection);

    // cross: enter selected directory
    if (pad.btn.cross == BTN_PRESSED && hasSelection && entries[selectedIndex].type == FILE_TYPE_FOLDER) {
        playSfxOnce(clickSfx);
        pushNavHistory();
        char next[MAX_PATH_LEN];
        joinPath(next, MAX_PATH_LEN, currentPath, entries[selectedIndex].name);
        loadDir(next);
    }

    // circle: go up one directory
    if (pad.btn.circle == BTN_PRESSED && strlen(currentPath) > 1) {
        playSfxOnce(clickSfx);
        toParentPath(currentPath);
        loadDir(currentPath);
        popNavHistory();
    }

    if (labelsStale) rebuildLabels();
    updateFolderSizer(&sizerSource);
}

void drawFileList(void)
{
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
            moveSlice(&hover, 47, listY + i * listRowHeight);
            drawSlice(&hover);
        }

        // draw checkbox
        if (entries[idx].checked)
            drawImage(&checkedBoxes[i]);
        else
            drawImage(&checkboxes[i]);

        // draw type icon
        int iconY = listY + i * listRowHeight + (listRowHeight - 43) / 2;
        moveImage(&fileIcons[entries[idx].type], 120, iconY);
        drawImage(&fileIcons[entries[idx].type]);

        // draw labels
        drawLabel(&labels[i]);
        drawLabel(&sizeLabels[i]);
        drawLabel(&typeLabels[i]);
    }

    drawLabel(&counterLabel);
}

void termFileList(void)
{
    cancelFolderSizer();
    free(entries);
    entries = NULL;
    entryCount = 0;
    entryCapacity = 0;
}

// --- action menu queries -----------------------------------------------------

static SelectionSummary summary;

const SelectionSummary *getSelectionSummary(void)
{
    // count checked rows; remember the last one for the single-check case
    int checkedCount = 0;
    int lastChecked  = -1;
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].checked) { checkedCount++; lastChecked = i; }
    }

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
        char sizeBuf[16];
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
        char sizeBuf[16];
        formatSizeApprox(e->size, e->approx, sizeBuf);
        const char *plus = e->approx ? "+" : "";
        snprintf(detail, sizeof(detail), "%d files%s, %s", e->fileCount, plus, sizeBuf);
    } else {
        formatSize(e->size, detail);
    }
    summary.title    = e->name;
    summary.subtitle = fileTypeName(e->type);
    summary.detail   = detail;
    summary.icon     = spriteRegions[fileTypeSprite(e->type)];
    return &summary;
}

const SelectionAction *getAvailableActions(int *outCount)
{
    static const SelectionAction list[] = {
        ACTION_COPY, ACTION_CUT, ACTION_PASTE, ACTION_DELETE, ACTION_RENAME,
        ACTION_NEW_FILE, ACTION_NEW_DIR, ACTION_EDIT, ACTION_PROPERTIES,
    };
    *outCount = (int)(sizeof(list) / sizeof(list[0]));
    return list;
}
