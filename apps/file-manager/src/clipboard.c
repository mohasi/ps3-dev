// clipboard - cut/copy path set plus the move/copy file operations.
#include "clipboard.h"
#include "file.h"
#include "string-utilities.h"
#include <stdlib.h>

#define COPY_BUF_SIZE (64 * 1024)

static ClipboardMode mode;
static char (*paths)[MAX_PATH_LEN];
static int count;
static int capacity;
static char copyBuf[COPY_BUF_SIZE];

// grows storage to hold at least n paths. returns 0 on allocation failure.
static int reserve(int n)
{
    if (n <= capacity) return 1;
    int cap = capacity > 0 ? capacity : 64;
    while (cap < n) cap *= 2;
    void *grown = realloc(paths, (size_t)cap * MAX_PATH_LEN);
    if (!grown) return 0;
    paths = grown;
    capacity = cap;
    return 1;
}

void clipboardBegin(ClipboardMode m)
{
    mode = m;
    count = 0;
}

int clipboardAdd(const char *absPath)
{
    if (!reserve(count + 1)) return -1;
    strCopy(paths[count++], MAX_PATH_LEN, absPath);
    return 0;
}

void clipboardClear(void)
{
    mode = CLIP_NONE;
    count = 0;
}

void clipboardTerm(void)
{
    free(paths);
    paths = NULL;
    count = 0;
    capacity = 0;
    mode = CLIP_NONE;
}

int clipboardIsEmpty(void) { return count == 0; }

int clipboardContains(const char *path)
{
    for (int i = 0; i < count; i++)
        if (strEq(paths[i], path)) return 1;
    return 0;
}

// writes destDir/<name> into out, but if that already exists, finds the lowest
// n >= 1 such that "<stem> (n)<ext>" is free and uses that instead. for files
// the extension (text after the last interior dot) is preserved; directories
// keep their whole name as the stem. used for copy, so duplicates land beside
// the original as "report (1).txt", "report (2).txt", ...
static void uniqueDest(char *out, int cap, const char *destDir, const char *name, int isDirEntry)
{
    joinPath(out, cap, destDir, name);
    if (!fileExists(out)) return;

    // split name into stem + extension (last interior dot; never a leading dot).
    int dot = -1;
    if (!isDirEntry)
        for (int i = strLen(name) - 1; i > 0; i--)
            if (name[i] == '.') { dot = i; break; }

    char stem[MAX_PATH_LEN];
    strCopy(stem, sizeof stem, name);
    const char *ext = "";
    if (dot >= 0) { stem[dot] = '\0'; ext = name + dot; }  // ext keeps its '.'

    char cand[MAX_PATH_LEN];
    for (int n = 1; ; n++) {
        int o = 0;
        appendStr(cand, sizeof cand, &o, stem);
        appendStr(cand, sizeof cand, &o, " (");
        o += intToDec(n, cand + o);
        appendStr(cand, sizeof cand, &o, ")");
        appendStr(cand, sizeof cand, &o, ext);
        cand[o] = '\0';
        joinPath(out, cap, destDir, cand);
        if (!fileExists(out)) return;
    }
}

void clipboardPasteInto(const char *destDir)
{
    int copying = (mode == CLIP_COPY);

    char dst[MAX_PATH_LEN];
    for (int i = 0; i < count; i++) {
        const char *name = baseName(paths[i]);

        if (copying) {
            // copies never overwrite: collisions (including pasting into the
            // source dir) get a "<name> (n)" duplicate beside the original.
            uniqueDest(dst, MAX_PATH_LEN, destDir, name, isDir(paths[i]));
            copyTree(paths[i], dst, copyBuf, COPY_BUF_SIZE);
            continue;
        }

        joinPath(dst, MAX_PATH_LEN, destDir, name);
        if (strEq(paths[i], dst)) continue;          // moving into own dir: no-op
        if (fileExists(dst)) deleteTree(dst, NULL);  // move overwrites on collision
        moveTree(paths[i], dst, copyBuf, COPY_BUF_SIZE);
    }

    clipboardClear();
}
