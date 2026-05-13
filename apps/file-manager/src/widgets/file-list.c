// file-list - scrollable directory listing widget
#include "widgets/file-list.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/slice.h"
#include "ui/breadcrumb.h"
#include "sprites.h"
#include "file.h"
#include "pad.h"
#include <string.h>
#include <sys/sys_time.h>

#define NAME_LEN           256
#define INITIAL_CAPACITY   256
#define HOLD_INITIAL_DELAY 300000  // microseconds before first repeat
#define HOLD_REPEAT_RATE    60000  // microseconds between subsequent repeats
#define SCROLL_NONE         0
#define SCROLL_DOWN         1
#define SCROLL_UP          -1

typedef enum {
    FILE_TYPE_FOLDER,
    FILE_TYPE_TEXT,
    FILE_TYPE_AUDIO,
    FILE_TYPE_GENERIC
} FileType;

typedef struct {
    char name[NAME_LEN];
    uint64_t size;
    FileType type;
    int checked;
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
static int selectionHistory[16];
static int scrollHistory[16];
static int historyDepth;
static uint64_t holdTimer;
static int holdRepeats;  // how many repeats have fired during current hold

static Image checkboxes[FILE_LIST_PAGE_SIZE];
static Image checkedBoxes[FILE_LIST_PAGE_SIZE];
static Image folderIcon;
static Image genericFileIcon;
static Image textFileIcon;
static Image audioFileIcon;
static Slice separators[FILE_LIST_PAGE_SIZE];
static Slice hover;
static Breadcrumb *breadcrumb;

static FileType classifyFileType(const char *name, int isDir)
{
    if (isDir) return FILE_TYPE_FOLDER;
    const char *ext = getExtension(name);
    if (ext) {
        static const char *textExts[] = {
            "txt", "xml", "json", "ini", "cfg", "conf", "log",
            "md", "csv", "htm", "html", "yaml", "yml", NULL
        };
        for (int i = 0; textExts[i]; i++) {
            if (compareStringNoCase(ext, textExts[i]) == 0) return FILE_TYPE_TEXT;
        }
        static const char *audioExts[] = {
            "mp3", "wav", "flac", "ogg", "aac", "wma", "at3", "m4a", NULL
        };
        for (int i = 0; audioExts[i]; i++) {
            if (compareStringNoCase(ext, audioExts[i]) == 0) return FILE_TYPE_AUDIO;
        }
    }
    return FILE_TYPE_GENERIC;
}

static void sortEntries(void)
{
    for (int i = 0; i < entryCount - 1; i++) {
        for (int j = i + 1; j < entryCount; j++) {
            int swap = 0;
            int iIsDir = entries[i].type == FILE_TYPE_FOLDER;
            int jIsDir = entries[j].type == FILE_TYPE_FOLDER;
            if (iIsDir < jIsDir) swap = 1;
            else if (iIsDir == jIsDir && compareStringNoCase(entries[i].name, entries[j].name) > 0) swap = 1;
            if (swap) {
                FileEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static void loadFileListDir(const char *path)
{
    strncpy(currentPath, path, MAX_PATH_LEN - 1);
    currentPath[MAX_PATH_LEN - 1] = '\0';
    entryCount = 0;
    selectedIndex = 0;
    scrollOffset = 0;

    // open directory
    int fd;
    if (cellFsOpendir(path, &fd) != CELL_FS_SUCCEEDED) {
        labelsStale = 1;
        return;
    }

    // read entries
    CellFsDirent ent;
    uint64_t readBytes;
    while (cellFsReaddir(fd, &ent, &readBytes) == CELL_FS_SUCCEEDED && readBytes > 0) {
        if (ent.d_name[0] == '.') continue;

        // grow buffer if needed
        if (entryCount >= entryCapacity) {
            int newCap = entryCapacity * 2;
            FileEntry *grown = (FileEntry *)realloc(entries, newCap * sizeof(FileEntry));
            if (!grown) break;
            entries = grown;
            entryCapacity = newCap;
        }

        // store name
        strncpy(entries[entryCount].name, ent.d_name, NAME_LEN - 1);
        entries[entryCount].name[NAME_LEN - 1] = '\0';
        entries[entryCount].checked = 0;

        // stat for type and size
        char full[MAX_PATH_LEN];
        joinPath(full, MAX_PATH_LEN, path, ent.d_name);

        CellFsStat st;
        if (cellFsStat(full, &st) == CELL_FS_SUCCEEDED) {
            int isDir = (st.st_mode & CELL_FS_S_IFDIR) != 0;
            entries[entryCount].type = classifyFileType(ent.d_name, isDir);
            entries[entryCount].size = isDir ? 0 : st.st_size;
        } else {
            entries[entryCount].type = FILE_TYPE_GENERIC;
            entries[entryCount].size = 0;
        }
        entryCount++;
    }
    cellFsClosedir(fd);

    sortEntries();
    labelsStale = 1;

    if (breadcrumb) setBreadcrumbPath(breadcrumb, currentPath);
}

static void updateCheckedCounter(void)
{
    int checked = 0;
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].checked) checked++;
    }
    char buf[32];
    int p = intToStr(checked, buf);
    buf[p++] = ' '; buf[p++] = '/'; buf[p++] = ' ';
    p += intToStr(entryCount, buf + p);
    buf[p] = '\0';
    setLabelText(&counterLabel, buf);
}

static void rebuildLabels(void)
{
    for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
        int idx = scrollOffset + i;
        if (idx < entryCount) {
            setLabelText(&labels[i], entries[idx].name);
            setLabelText(&typeLabels[i], entries[idx].type == FILE_TYPE_FOLDER ? "Folder" : "File");
            if (entries[idx].type == FILE_TYPE_FOLDER) {
                setLabelText(&sizeLabels[i], "\xe2\x80\x94"); // em dash (U+2014)
            } else {
                char sizeBuf[16];
                formatSize(entries[idx].size, sizeBuf);
                setLabelText(&sizeLabels[i], sizeBuf);
            }
        } else {
            setLabelText(&labels[i], "");
            setLabelText(&sizeLabels[i], "");
            setLabelText(&typeLabels[i], "");
        }
    }
    updateCheckedCounter();
    labelsStale = 0;
}

void initFileList(Font *font, GfxTexture spritesheet, int x, int y, int maxWidth, int rowHeight, int fontSize, uint32_t color, Breadcrumb *bc)
{
    breadcrumb = bc;
    listY = y;
    listRowHeight = rowHeight;
    entryCount = 0;
    entryCapacity = INITIAL_CAPACITY;
    entries = (FileEntry *)malloc(entryCapacity * sizeof(FileEntry));
    selectedIndex = 0;
    scrollOffset = 0;
    labelsStale = 1;
    historyDepth = 0;

    initSlice(&hover, spritesheet, 47, y, 1882 - 47, rowHeight, sprites[SPRITE_HOVER], 8);
    initLabel(&counterLabel, font, 55, 953, 200, AUTO, 20, color, TEXT_NOWRAP, NULL);

    // type icons (single instances, repositioned per row at draw time)
    initImage(&folderIcon, spritesheet, 120, 0, 40, 34, sprites[SPRITE_FOLDER_ICON], GFX_FILTER_LINEAR);
    initImage(&genericFileIcon, spritesheet, 120, 0, 34, 43, sprites[SPRITE_GENERIC_FILE_ICON], GFX_FILTER_LINEAR);
    initImage(&textFileIcon, spritesheet, 120, 0, 34, 43, sprites[SPRITE_TEXT_FILE_ICON], GFX_FILTER_LINEAR);
    initImage(&audioFileIcon, spritesheet, 120, 0, 34, 43, sprites[SPRITE_AUDIO_FILE_ICON], GFX_FILTER_LINEAR);

    for (int i = 0; i < FILE_LIST_PAGE_SIZE; i++) {
        int ry = y + i * rowHeight;
        int cy = ry + (rowHeight - 25) / 2;

        initLabel(&labels[i], font, x, ry + 25, maxWidth, AUTO, fontSize, color, TEXT_NOWRAP_ELLIPSIS, NULL);
        initLabel(&sizeLabels[i], font, 1467, ry + 25, 200, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
        initLabel(&typeLabels[i], font, 1722, ry + 25, 150, AUTO, fontSize, color, TEXT_NOWRAP, NULL);
        initImage(&checkboxes[i], spritesheet, 71, cy, 25, 25, sprites[SPRITE_CHECKBOX], GFX_FILTER_LINEAR);
        initImage(&checkedBoxes[i], spritesheet, 71, cy, 25, 25, sprites[SPRITE_CHECKBOX_CHECKED], GFX_FILTER_LINEAR);
        initSlice(&separators[i], spritesheet, 47, ry - 1, 1884 - 47, 2, sprites[SPRITE_SEPARATOR], 1);
    }

    loadFileListDir("/");
}

// checks if a button press or hold should trigger a scroll step.
// on first press, fires immediately and resets the repeat timer.
// on sustained hold, fires after an initial delay then at a faster repeat rate.
static int checkButtonRepeat(uint8_t state, int direction, int *scrollDirection)
{
    if (state == BTN_PRESSED) {
        *scrollDirection = direction;
        holdTimer = sys_time_get_system_time();
        holdRepeats = 0;
        return 1;
    }
    if (state == BTN_HELD) {
        uint64_t now = sys_time_get_system_time();
        uint64_t elapsed = now - holdTimer;
        uint64_t threshold = holdRepeats == 0 ? HOLD_INITIAL_DELAY : HOLD_REPEAT_RATE;
        if (elapsed >= threshold) {
            *scrollDirection = direction;
            holdRepeats++;
            holdTimer = now;
            return 1;
        }
    }
    return 0;
}

void updateFileList(void)
{
    int scrollDirection = SCROLL_NONE;
    checkButtonRepeat(pad.btn.down, SCROLL_DOWN, &scrollDirection);
    checkButtonRepeat(pad.btn.up, SCROLL_UP, &scrollDirection);

    int canScrollDown = entryCount > 0 && selectedIndex < entryCount - 1;
    int canScrollUp = entryCount > 0 && selectedIndex > 0;

    if (scrollDirection == SCROLL_DOWN && canScrollDown) {
        selectedIndex++;
        if (selectedIndex >= scrollOffset + FILE_LIST_PAGE_SIZE) {
            scrollOffset = selectedIndex - FILE_LIST_PAGE_SIZE + 1;
            labelsStale = 1;
        }
    }
    if (scrollDirection == SCROLL_UP && canScrollUp) {
        selectedIndex--;
        if (selectedIndex < scrollOffset) {
            scrollOffset = selectedIndex;
            labelsStale = 1;
        }
    }

    int hasSelection = selectedIndex >= 0 && selectedIndex < entryCount;

    // square: toggle checked state
    if (pad.btn.square == BTN_PRESSED && hasSelection) {
        entries[selectedIndex].checked = !entries[selectedIndex].checked;
        labelsStale = 1;
    }

    // cross: enter selected directory
    if (pad.btn.cross == BTN_PRESSED && hasSelection && entries[selectedIndex].type == FILE_TYPE_FOLDER) {
        if (historyDepth < 16) {
            selectionHistory[historyDepth] = selectedIndex;
            scrollHistory[historyDepth] = scrollOffset;
            historyDepth++;
        }
        char next[MAX_PATH_LEN];
        joinPath(next, MAX_PATH_LEN, currentPath, entries[selectedIndex].name);
        loadFileListDir(next);
    }

    // circle: go up one directory
    if (pad.btn.circle == BTN_PRESSED && strlen(currentPath) > 1) {
        int len = strlen(currentPath);
        if (len > 1 && currentPath[len - 1] == '/') len--;
        while (len > 1 && currentPath[len - 1] != '/') len--;
        if (len <= 1) {
            currentPath[0] = '/';
            currentPath[1] = '\0';
        } else {
            currentPath[len] = '\0';
        }
        loadFileListDir(currentPath);
        if (historyDepth > 0) {
            historyDepth--;
            selectedIndex = selectionHistory[historyDepth];
            scrollOffset = scrollHistory[historyDepth];
            if (selectedIndex >= entryCount) selectedIndex = entryCount > 0 ? entryCount - 1 : 0;
            if (scrollOffset > selectedIndex) scrollOffset = selectedIndex;
        }
    }

    if (labelsStale) rebuildLabels();
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
        int iconY = listY + i * listRowHeight;
        switch (entries[idx].type) {
            case FILE_TYPE_FOLDER:
                moveImage(&folderIcon, 120, iconY + (listRowHeight - 40) / 2 + 2);
                drawImage(&folderIcon);
                break;
            case FILE_TYPE_TEXT:
                moveImage(&textFileIcon, 120, iconY + (listRowHeight - 43) / 2);
                drawImage(&textFileIcon);
                break;
            case FILE_TYPE_AUDIO:
                moveImage(&audioFileIcon, 120, iconY + (listRowHeight - 43) / 2);
                drawImage(&audioFileIcon);
                break;
            default:
                moveImage(&genericFileIcon, 120, iconY + (listRowHeight - 43) / 2);
                drawImage(&genericFileIcon);
                break;
        }

        // draw labels
        drawLabel(&labels[i]);
        drawLabel(&sizeLabels[i]);
        drawLabel(&typeLabels[i]);
    }

    drawLabel(&counterLabel);
}

void termFileList(void)
{
    free(entries);
    entries = NULL;
    entryCount = 0;
    entryCapacity = 0;
}
