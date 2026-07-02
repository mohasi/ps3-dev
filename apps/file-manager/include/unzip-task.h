#pragma once

// unzip-task - extracts one archive into a destination directory on the file-task worker.
// file-list sets the source archive and destination dir, then launches runUnzip via the
// progress overlay.

// sets the archive the next runUnzip() extracts. call on the main thread before launching.
void setUnzipSource(const char *zipPath);

// sets the directory the next runUnzip() extracts into (created if absent). call on the
// main thread before launching.
void setUnzipDest(const char *destDir);

// chooses how runUnzip resolves file-leaf collisions in destDir: 1 replace, 0 keep. set on
// the main thread before launching.
void setUnzipReplaceOnConflict(int replace);

// task body: extracts the source archive into destDir, reporting bytes and honouring cancel.
void runUnzip(void);

// 1 if the last runUnzip() failed outright (not just cancelled). read on the main thread by
// the finisher after the task ends. cleared at the start of each runUnzip().
int unzipHadError(void);
