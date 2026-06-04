#pragma once

// clipboard - holds a set of absolute paths marked for a move (cut) or a
// duplicate (copy), plus the file operations that carry them out. the state
// persists across directory navigation until pasted or replaced. file-list
// drives this: it gathers the selected paths (with the sizes it already knows)
// and reloads the listing; the clipboard owns the paths and the actual
// rename/copy/delete work.

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

// sets the directory a subsequent pasteClipboardContents() will paste into.
// call on the main thread before launching the task.
void setPasteDest(const char *destDir);

// task body: pastes into the dest set by setPasteDest, reporting bytes and
// honouring cancel (reusing carried sizes, walking only lower-bound items).
// copies never overwrite (collisions become "<name> (n)"); cuts move and
// overwrite a same-named target. on cancel it drops the partial destination,
// and for a move keeps the source unless the copy fully landed. does not clear
// the clipboard - the finisher does, on the main thread.
void pasteClipboardContents(void);
