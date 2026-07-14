// folder-sizer - see folder-sizer.h
#include "folder-sizer.h"
#include "tree-walk.h"
#include "vfs.h"
#include "thread.h"
#include <sys/sys_time.h>

#define SIZE_BUDGET_US 500000  // per-folder walker budget; longer trees report approximate

// one walker at a time. cancel is raised by the main thread and lowered by
// the next updateFolderSizer() once the previous worker has exited, so we
// never start a new walker before the old one has stopped touching the
// caller's storage.
static volatile int cancel;
static volatile int busy;

// per-folder accumulation for one walkTree pass. the deadline caps how long a single folder's walk
// runs; going over it stops the walk and the result is reported as approximate.
typedef struct { int files; uint64_t bytes; uint64_t deadline; } SizeAccum;

static WalkResult sizeVisit(const char *fullPath, const char *name, VfsEntryType type, void *ctx)
{
   (void)name;
   SizeAccum *accum = (SizeAccum *)ctx;
   if (type == VFS_ENTRY_FILE) {
      VfsStat st;
      if (statPath(fullPath, &st) == 0) { accum->files++; accum->bytes += st.size; }
   }
   return sys_time_get_system_time() > accum->deadline ? WALK_STOP : WALK_CONTINUE;
}

static void worker(uint64_t arg)
{
   const FolderSizeCallbacks *callbacks = (const FolderSizeCallbacks *)(uintptr_t)arg;
   int count = callbacks->count();
   char path[MAX_PATH_LEN];

   for (int i = 0; i < count && !cancel; i++) {
      int generation = 0;
      if (!callbacks->needsSizing(i, path, MAX_PATH_LEN, &generation)) continue;

      SizeAccum accum = { 0, 0, sys_time_get_system_time() + SIZE_BUDGET_US };
      walkTree(path, sizeVisit, &accum, &cancel);

      if (cancel) break;
      callbacks->applyResult(i, accum.bytes, accum.files, sys_time_get_system_time() > accum.deadline, generation);
   }
   busy = 0;
   exitThread();
}

void updateFolderSizer(const FolderSizeCallbacks *callbacks)
{
   if (busy) return;   // previous walker still draining; will exit on its own
   cancel = 0;         // safe now -- no walker is running

   int count = callbacks->count();
   for (int i = 0; i < count; i++) {
      char path[MAX_PATH_LEN];
      int generation = 0;
      if (!callbacks->needsSizing(i, path, MAX_PATH_LEN, &generation)) continue;
      busy = 1;
      sys_ppu_thread_t tid;
      if (spawnThread(&tid, worker, (uint64_t)(uintptr_t)callbacks, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "folder-sizer") != 0)
         busy = 0;   // spawn failed: no walker is running, so don't leave busy stuck
      return;
   }
}

// Signals the walker to bail. Non-blocking: the next updateFolderSizer() won't
// start a new walker until this one clears `busy`, and stale results are dropped
// by the generation check, so a directory change need not wait here.
void cancelFolderSizer(void)
{
   cancel = 1;
}
