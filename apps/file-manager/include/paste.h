#pragma once

// paste - carries the clipboard's items into a destination directory. it reads
// the cut/copy set through the clipboard getters (clipboard.h) and is the only
// place the move/copy file work lives. file-list sets the destination, then
// launches runPaste on the file-task worker via the progress overlay.

// sets the directory the next runPaste() pastes into. call on the main thread
// before launching the task.
void setPasteDest(const char *destDir);

// chooses how runPaste resolves file-leaf collisions when merging into an
// existing destination: replace != 0 overwrites the destination file, replace == 0
// keeps it. set on the main thread before launching. it has no effect where there
// is no collision - a copy back into the source dir still gets a "<name> (n)"
// duplicate, and a brand-new destination is just created.
void setPasteReplaceOnConflict(int replace);

// counts the existing files the pending paste would land on (how many a merge
// would touch), so the caller can decide whether and how to prompt. returns 0, 1,
// or up to cap (counting stops there). call after setPasteDest, on the main
// thread, before launching. items pasted back into their own directory are
// excluded (a copy is suffixed, a move is a no-op), so they never count.
int  countClipboardConflicts(int cap);

// task body: pastes the clipboard into the dest set by setPasteDest, reporting
// bytes and honouring cancel (reusing carried sizes, walking only lower-bound
// items). a copy back into the source dir duplicates as "<name> (n)"; otherwise a
// pre-existing destination is merged into (file leaves replaced or kept per
// setPasteReplaceOnConflict) and a brand-new one is created. a move deletes the source once
// its copy has fully landed. on cancel it drops a partial fresh copy and keeps the
// source. does not clear the clipboard - the finisher does, on the main thread.
void runPaste(void);
