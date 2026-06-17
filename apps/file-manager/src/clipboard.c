// clipboard - cut/copy path set (see clipboard.h). pure storage; the paste
// module reads it through the getters to carry the items across.
#include "clipboard.h"
#include "file.h"
#include "string-utilities.h"
#include "dynarray.h"
#include <stdlib.h>

typedef struct {
   char     path[MAX_PATH_LEN];
   uint64_t size;   // bytes; a lower bound when exact == 0
   int      exact;  // 1 if size is known exactly
} ClipItem;

static ClipboardMode mode;
static ClipItem     *items;
static int           count;
static int           capacity;

void beginClipboard(ClipboardMode m)
{
   mode = m;
   count = 0;
}

int addToClipboard(const char *absPath, uint64_t size, int exact)
{
   if (!growArray(items, &capacity, count + 1)) return -1;
   strCopy(items[count].path, MAX_PATH_LEN, absPath);
   items[count].size  = size;
   items[count].exact = exact;
   count++;
   return 0;
}

void clearClipboard(void)
{
   mode = CLIP_NONE;
   count = 0;
}

void freeClipboard(void)
{
   free(items);
   items = NULL;
   count = 0;
   capacity = 0;
   mode = CLIP_NONE;
}

int isClipboardEmpty(void) { return count == 0; }

int isOnClipboard(const char *path)
{
   for (int i = 0; i < count; i++)
      if (strEq(items[i].path, path)) return 1;
   return 0;
}

ClipboardMode getClipboardMode(void) { return mode; }

int         getClipboardCount(void)          { return count; }
const char *getClipboardPath(int index)      { return items[index].path; }
uint64_t    getClipboardSize(int index)      { return items[index].size; }
int         isClipboardSizeExact(int index) { return items[index].exact; }
