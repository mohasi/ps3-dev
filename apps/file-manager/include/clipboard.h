#pragma once

// clipboard - holds a set of absolute paths marked for a move (cut) or a
// duplicate (copy). pure storage: it remembers what was cut/copied and its
// known sizes, and persists across directory navigation until pasted or
// replaced. the paste module reads it through the getters below to carry the
// items across; file-list gathers the selection into it and queries it to
// ghost rows pending a paste.

#include <stdint.h>

typedef enum {
    CLIP_NONE,
    CLIP_CUT,
    CLIP_COPY
} ClipboardMode;

// starts a fresh cut or copy, discarding any previous clipboard contents.
// follow with addToClipboard() calls to populate it.
void beginClipboard(ClipboardMode mode);

// appends one absolute path plus its byte size. exact is 1 when the size is
// known exactly (a file, or a fully-walked folder) and 0 when it is only a
// lower bound (an unsized or budget-truncated folder). returns 0, or -1 on
// allocation failure.
int addToClipboard(const char *absPath, uint64_t size, int exact);

// resets the clipboard to empty (CLIP_NONE); retains backing storage.
void clearClipboard(void);

// frees backing storage. call at shutdown.
void freeClipboard(void);

int isClipboardEmpty(void);

// non-zero when path is on the clipboard (cut or copy). used to ghost any item
// pending a paste.
int isOnClipboard(const char *path);

// the current clipboard mode (CLIP_NONE / CLIP_CUT / CLIP_COPY).
ClipboardMode getClipboardMode(void);

// read access for the paste module: the count of held items and, per index,
// the absolute path, its byte size, and whether that size is exact (vs a lower
// bound that paste must walk to total up).
int         getClipboardCount(void);
const char *getClipboardPath(int index);
uint64_t    getClipboardSize(int index);
int         isClipboardSizeExact(int index);
