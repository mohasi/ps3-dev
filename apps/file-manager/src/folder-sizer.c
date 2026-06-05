// folder-sizer - see folder-sizer.h
#include "folder-sizer.h"
#include "file.h"
#include "thread.h"
#include <string.h>
#include <sys/sys_time.h>

#define SIZE_BUDGET_US 500000  // per-folder walker budget; longer trees report approximate
#define WALK_DEPTH_MAX 64

// one walker at a time. cancel is raised by the main thread and lowered by
// the next updateFolderSizer() once the previous worker has exited, so we
// never start a new walker before the old one has stopped touching the
// caller's storage.
static volatile int cancel;
static volatile int busy;

static int shouldStop(uint64_t deadline)
{
    return cancel || sys_time_get_system_time() > deadline;
}

// iterative directory walk. one shared path buffer is mutated as we descend
// (append "/name" on push, truncate back to the parent length on pop), so
// stack usage stays flat regardless of depth. fdStack/lenStack hold the
// open dir handle and the parent-path length for each level.
static void walkPath(char *path, int pathLen, int *files, uint64_t *bytes, uint64_t deadline)
{
    int fdStack[WALK_DEPTH_MAX];
    int lenStack[WALK_DEPTH_MAX];
    int top = 0;

    CellFsStat st;
    if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) return;
    if (!(st.st_mode & CELL_FS_S_IFDIR)) {
        (*files)++;
        *bytes += st.st_size;
        return;
    }
    if (cellFsOpendir(path, &fdStack[0]) != CELL_FS_SUCCEEDED) return;
    lenStack[0] = pathLen;
    top = 1;

    CellFsDirent ent;
    uint64_t read;

    while (top > 0) {
        if (shouldStop(deadline)) break;

        if (cellFsReaddir(fdStack[top - 1], &ent, &read) != CELL_FS_SUCCEEDED || read == 0) {
            cellFsClosedir(fdStack[top - 1]);
            top--;
            if (top > 0) path[lenStack[top]] = '\0';
            continue;
        }
        if (ent.d_name[0] == '.') continue;

        int parentLen = lenStack[top - 1];
        path[parentLen] = '\0';
        int nameLen = (int)strlen(ent.d_name);
        int needsSlash = parentLen > 0 && path[parentLen - 1] != '/';
        int childLen = parentLen + (needsSlash ? 1 : 0) + nameLen;
        if (childLen >= MAX_PATH_LEN) continue;
        if (needsSlash) path[parentLen++] = '/';
        memcpy(path + parentLen, ent.d_name, nameLen);
        path[childLen] = '\0';

        if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) {
            path[lenStack[top - 1]] = '\0';
            continue;
        }
        if (!(st.st_mode & CELL_FS_S_IFDIR)) {
            (*files)++;
            *bytes += st.st_size;
            path[lenStack[top - 1]] = '\0';
            continue;
        }
        if (top >= WALK_DEPTH_MAX || cellFsOpendir(path, &fdStack[top]) != CELL_FS_SUCCEEDED) {
            path[lenStack[top - 1]] = '\0';
            continue;
        }
        lenStack[top] = childLen;
        top++;
    }

    while (top > 0) cellFsClosedir(fdStack[--top]);
}

static void worker(uint64_t arg)
{
    const FolderSizeCallbacks *callbacks = (const FolderSizeCallbacks *)(uintptr_t)arg;
    int count = callbacks->count();
    char path[MAX_PATH_LEN];

    for (int i = 0; i < count && !cancel; i++) {
        if (!callbacks->needsSizing(i, path, MAX_PATH_LEN)) continue;

        int files = 0;
        uint64_t bytes = 0;
        uint64_t deadline = sys_time_get_system_time() + SIZE_BUDGET_US;
        walkPath(path, (int)strlen(path), &files, &bytes, deadline);

        if (cancel) break;
        callbacks->applyResult(i, bytes, files, sys_time_get_system_time() > deadline);
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
        if (!callbacks->needsSizing(i, path, MAX_PATH_LEN)) continue;
        busy = 1;
        sys_ppu_thread_t tid;
        spawnThread(&tid, worker, (uint64_t)(uintptr_t)callbacks, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "folder-sizer");
        return;
    }
}

void cancelFolderSizer(void)
{
    cancel = 1;
}
