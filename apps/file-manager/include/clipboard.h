#pragma once

// clipboard - holds a set of absolute paths marked for a move (cut) or a
// duplicate (copy), plus the file operations that carry them out. the state
// persists across directory navigation until pasted or replaced. file-list
// drives this: it gathers the selected paths and reloads the listing; the
// clipboard owns the paths and the actual rename/copy/delete work.

typedef enum {
    CLIP_NONE,
    CLIP_CUT,
    CLIP_COPY
} ClipboardMode;

// starts a fresh cut or copy, discarding any previous clipboard contents.
// follow with clipboardAdd() calls to populate it.
void clipboardBegin(ClipboardMode mode);

// appends one absolute path to the clipboard. returns 0, or -1 on alloc failure.
int clipboardAdd(const char *absPath);

// resets the clipboard to empty (CLIP_NONE); retains backing storage.
void clipboardClear(void);

// frees backing storage. call at shutdown.
void clipboardTerm(void);

int clipboardIsEmpty(void);

// non-zero when path is on the clipboard (cut or copy). used to ghost any item
// pending a paste.
int clipboardContains(const char *path);

// carries the clipboard into destDir, then clears it. cut entries are moved,
// overwriting an existing same-named target; an entry already living in destDir
// is skipped. copy entries are never destructive: a name collision (including
// copying into the source dir) produces a "<name> (n)" duplicate beside the
// original. (a future variant can take a progress callback for a live overlay.)
void clipboardPasteInto(const char *destDir);
