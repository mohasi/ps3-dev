// file-task - background file operation engine: progress + cancel state and a
// worker thread (see file-task.h). the actual filesystem walking lives in the
// progress-reporting tree helpers in file.h; this just runs a task body on a
// thread and tracks how far it has got.
#include "file-task.h"
#include "thread.h"

// shared state: naturally-aligned scalars, each written by a single thread
// (cancel by the main thread, the counters + running by the worker), so plain
// volatile access is enough on the PPU - no lock needed. running flips to 0
// inside the worker entry once the body returns.
static volatile int      running;
static volatile int      cancel;
static volatile uint64_t processedBytes;
static volatile uint64_t totalBytes;
static TaskBody          body;

static void workerEntry(uint64_t arg)
{
    (void)arg;
    if (body) body();
    running = 0;
    exitThread();
}

void startTask(TaskBody taskBody)
{
    body           = taskBody;
    processedBytes = 0;
    totalBytes     = 0;
    cancel         = 0;
    running        = 1;
    // detached + LOW priority: the render loop stays responsive while the task
    // grinds through the filesystem, and there's nothing to join.
    sys_ppu_thread_t tid;
    if (spawnThread(&tid, workerEntry, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "file-task") != 0)
        running = 0;  // spawn failed: nothing to run, so the overlay closes cleanly
}

int      isTaskRunning(void)     { return running; }
void     cancelTask(void)        { cancel = 1; }
uint64_t getProcessedBytes(void) { return processedBytes; }
uint64_t getTotalBytes(void)     { return totalBytes; }

void     setTotalBytes(uint64_t t)     { totalBytes = t; }
void     addProcessedBytes(uint64_t b) { processedBytes += b; }
int      isCancelRequested(void)       { return cancel; }
