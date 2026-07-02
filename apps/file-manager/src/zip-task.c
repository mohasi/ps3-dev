// zip-task - path set + worker that zips it into one archive (see zip-task.h).
#include "zip-task.h"
#include "vfs.h"
#include "zip.h"
#include "string-utilities.h"
#include "dynarray.h"
#include "file-task.h"
#include <stdlib.h>

static char  destZipPath[MAX_PATH_LEN];
static char *paths;      // MAX_PATH_LEN-wide rows, indexed like a 2D array
static int   count;
static int   capacity;
static int   zipFailed;

void setZipDest(const char *dest) { strCopy(destZipPath, sizeof destZipPath, dest); }

void beginZip(void) { count = 0; }

int addToZip(const char *absPath)
{
   // growArray sizes by sizeof(*paths) == sizeof(char) == 1, so "needed" here must be a
   // byte count, not a row count: (count + 1) rows of MAX_PATH_LEN bytes each. Passing
   // count + 1 directly (as if elemSize were MAX_PATH_LEN) undersized the buffer by a
   // factor of MAX_PATH_LEN and corrupted the heap past the first couple of entries.
   if (!growArray(paths, &capacity, (count + 1) * MAX_PATH_LEN)) return -1;
   strCopy(paths + (size_t)count * MAX_PATH_LEN, MAX_PATH_LEN, absPath);
   count++;
   return 0;
}

void freeZip(void)
{
   free(paths);
   paths = NULL;
   count = 0;
   capacity = 0;
}

int zipHadError(void) { return zipFailed; }

// builds the flat char[MAX_PATH_LEN] rows into a pointer array zipPathsProgress/
// measureZipSource expect. caller frees the returned array.
static const char **buildRowPointers(void)
{
   const char **rows = (const char **)malloc((size_t)count * sizeof(char *));
   if (!rows) return NULL;
   for (int i = 0; i < count; i++) rows[i] = paths + (size_t)i * MAX_PATH_LEN;
   return rows;
}

static uint64_t measureZipSet(void)
{
   const char **rows = buildRowPointers();
   if (!rows) return 0;
   uint64_t total = measureZipSource(rows, count, isCancelRequested);
   free(rows);
   return total;
}

void runZip(void)
{
   zipFailed = 0;

   const char **rows = buildRowPointers();
   if (!rows) { zipFailed = 1; return; }

   setTotalBytes(measureZipSet());
   int r = zipPathsProgress(rows, count, destZipPath, addProcessedBytes, isCancelRequested);
   free(rows);
   if (r == -1) zipFailed = 1;
}
