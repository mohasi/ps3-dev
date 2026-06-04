#pragma once

// file-task - runs one long file operation on a background thread while the
// main loop keeps rendering, tracking byte progress and a cancel flag. only one
// task runs at a time (callers gate on isTaskRunning).
//
// state is a few naturally-aligned scalars, each written by a single thread, so
// plain volatile is enough on the PPU - no lock (as folder-sizer does too).
//
// a task body sets the total, then works via the progress-reporting tree helpers
// in file.h, passing addProcessedBytes / isCancelRequested as the callbacks.

#include <stdint.h>

typedef void (*TaskBody)(void);

// --- main thread ---------------------------------------------------------

void     startTask(TaskBody body);     // spawn the worker; resets progress + cancel
int      isTaskRunning(void);          // 1 until the task body returns
void     cancelTask(void);             // ask the task to stop at its next check
uint64_t getProcessedBytes(void);      // bytes processed so far
uint64_t getTotalBytes(void);          // total bytes to process

// --- task body -----------------------------------------------------------

void     setTotalBytes(uint64_t total);
void     addProcessedBytes(uint64_t bytes);  // pass as the onBytes callback
int      isCancelRequested(void);            // pass as the cancelled callback
