// delete - delete path set plus the removal worker (see delete.h).
#include "delete.h"
#include "vfs.h"
#include "file-task.h"
#include "string-utilities.h"
#include "dynarray.h"
#include <stdlib.h>

typedef struct {
   char     path[MAX_PATH_LEN];
   uint64_t size;   // bytes; a lower bound when exact == 0
   int      exact;  // 1 if size is known exactly
} DeleteItem;

static DeleteItem *items;
static int         count;
static int         capacity;

void beginDelete(void)
{
   count = 0;
}

int addToDelete(const char *absPath, uint64_t size, int exact)
{
   if (!growArray(items, &capacity, count + 1)) return -1;
   strCopy(items[count].path, MAX_PATH_LEN, absPath);
   items[count].size  = size;
   items[count].exact = exact;
   count++;
   return 0;
}

void freeDelete(void)
{
   free(items);
   items = NULL;
   count = 0;
   capacity = 0;
}

void runDelete(void)
{
   // phase 1: total bytes for the percentage - reuse known sizes, walk only
   // the items whose size is a lower bound.
   uint64_t total = 0;
   for (int i = 0; i < count && !isCancelRequested(); i++)
      total += items[i].exact ? items[i].size : measureTree(items[i].path, isCancelRequested);
   setTotalBytes(total);

   // phase 2: remove each target with progress + cancel checks.
   for (int i = 0; i < count; i++) {
      if (isCancelRequested()) break;
      deleteTreeProgress(items[i].path, addProcessedBytes, isCancelRequested);
   }
}
