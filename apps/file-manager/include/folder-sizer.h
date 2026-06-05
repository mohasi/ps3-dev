#pragma once

// folder-sizer - background recursive size+count walker for a list of folders.
//
// the caller (typically file-list) owns the storage and identifies entries by
// index. each frame it calls updateFolderSizer() with callbacks describing how
// to iterate; the sizer walks one unsized folder at a time on a worker thread
// under a per-folder time budget. results that hit the budget are flagged
// approx so the ui can show a trailing '+'.
//
// when the caller's storage changes (e.g. cd to a new directory) it must
// call cancelFolderSizer() first so the in-flight walker bails before the
// next updateFolderSizer() starts a new one.

#include <stdint.h>

typedef struct FolderSizeCallbacks {
    int  (*count)(void);                                          // total entries
    int  (*needsSizing)(int index, char *outPath, int cap);       // 1 if entry is an unsized folder; fills full path
    void (*applyResult)(int index, uint64_t bytes, int files, int approx);
} FolderSizeCallbacks;

void updateFolderSizer(const FolderSizeCallbacks *callbacks);
void cancelFolderSizer(void);
