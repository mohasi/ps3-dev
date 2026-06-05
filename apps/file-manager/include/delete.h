#pragma once

// delete - holds a set of absolute paths marked for deletion plus the worker
// that removes them. file-list gathers the selection into it (passing the
// sizes it already knows), then launches runDelete on the file-task worker
// via the progress overlay. delete owns the path set and the removal; it knows
// nothing about the listing.

#include <stdint.h>

// starts a fresh delete set, discarding any previous contents.
void beginDelete(void);

// appends one absolute path plus its byte size. exact is 1 when the size is
// known exactly, 0 when it is only a lower bound (walked on demand for the
// progress total). returns 0, or -1 on allocation failure.
int addToDelete(const char *absPath, uint64_t size, int exact);

// frees backing storage. call at shutdown.
void freeDelete(void);

// task body: deletes the gathered set, reporting bytes and honouring cancel.
// delete is atomic per item, so cancel just stops between items - nothing is
// left half-deleted.
void runDelete(void);
