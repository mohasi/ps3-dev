// unzip-task - extracts one archive into a destination dir (see unzip-task.h).
#include "unzip-task.h"
#include "vfs.h"
#include "zip.h"
#include "string-utilities.h"
#include "file-task.h"

static char srcZipPath[MAX_PATH_LEN];
static char destDir[MAX_PATH_LEN];
static int  replaceOnConflict = 1;
static int  unzipFailed;

void setUnzipSource(const char *zipPath)  { strCopy(srcZipPath, sizeof srcZipPath, zipPath); }
void setUnzipDest(const char *dir)        { strCopy(destDir, sizeof destDir, dir); }
void setUnzipReplaceOnConflict(int replace) { replaceOnConflict = replace ? 1 : 0; }

int unzipHadError(void) { return unzipFailed; }

void runUnzip(void)
{
   unzipFailed = 0;
   setTotalBytes(measureZipArchive(srcZipPath));
   int r = unzipArchiveProgress(srcZipPath, destDir, replaceOnConflict, addProcessedBytes, isCancelRequested);
   if (r == -1) unzipFailed = 1;
}
