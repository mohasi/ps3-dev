#pragma once

// tree-walk - shared iterative, cancellable depth-first directory walk. Visits every descendant of
// a root folder (files and folders) through a callback. One mutated path buffer is reused as it
// descends, so stack usage stays flat regardless of depth. Callers differ only in what they do per
// entry (accumulate size, match names, ...).
//
// The walk itself never stats: the entry type comes from readDir (a cheap directory-entry field, not
// a stat, on every backend), which is all that's needed to decide descent. A visitor that needs more
// (e.g. a file's size) stats fullPath itself, so it only pays for the entries it actually cares about.

#include "vfs.h"   // VfsEntryType, statPath, MAX_PATH_LEN

typedef enum { WALK_CONTINUE, WALK_STOP } WalkResult;

// called for each entry under the root. fullPath is the entry's path, name its basename, type its
// readDir kind (stat fullPath yourself if you need size/mtime). return WALK_STOP to abort the whole
// walk (e.g. a result cap or time budget hit); WALK_CONTINUE to keep going - dirs are descended into.
typedef WalkResult (*WalkVisit)(const char *fullPath, const char *name, VfsEntryType type, void *ctx);

// walks root's subtree depth-first (root itself is not visited). aborts if *cancel becomes non-zero;
// cancel may be NULL for an uninterruptible walk.
void walkTree(const char *root, WalkVisit visit, void *ctx, volatile int *cancel);
