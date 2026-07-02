#pragma once

// zip-task - holds a set of absolute paths marked for compression plus the worker that
// bundles them into one archive. file-list gathers the selection into it (paths only; sizes
// come from zipPathsProgress's own progress reporting), sets the destination archive path,
// then launches runZip on the file-task worker via the progress overlay.

#include <stdint.h>

// sets the archive file the next runZip() creates. call on the main thread before launching.
void setZipDest(const char *destZipPath);

// starts a fresh zip set, discarding any previous contents.
void beginZip(void);

// appends one absolute path (file or directory, walked recursively by the task body).
// returns 0, or -1 on allocation failure.
int addToZip(const char *absPath);

// frees backing storage. call at shutdown.
void freeZip(void);

// task body: zips the gathered set into the destination set by setZipDest, reporting bytes
// and honouring cancel.
void runZip(void);

// 1 if the last runZip() failed outright (not just cancelled). read on the main thread by the
// finisher after the task ends. cleared at the start of each runZip().
int zipHadError(void);
