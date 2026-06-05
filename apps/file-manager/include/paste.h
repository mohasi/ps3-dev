#pragma once

// paste - carries the clipboard's items into a destination directory. it reads
// the cut/copy set through the clipboard getters (clipboard.h) and is the only
// place the move/copy file work lives. file-list sets the destination, then
// launches runPaste on the file-task worker via the progress overlay.

// sets the directory the next runPaste() pastes into. call on the main thread
// before launching the task.
void setPasteDest(const char *destDir);

// task body: pastes the clipboard into the dest set by setPasteDest, reporting
// bytes and honouring cancel (reusing carried sizes, walking only lower-bound
// items). copies never overwrite (collisions become "<name> (n)"); cuts move
// and overwrite a same-named target. on cancel it drops the partial
// destination, and for a move keeps the source unless the copy fully landed.
// does not clear the clipboard - the finisher does, on the main thread.
void runPaste(void);
