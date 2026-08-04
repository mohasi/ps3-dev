// video-export - registers finished downloads with the system's video database (see video-export.h).

#include "video-export.h"

#include "thread.h"             // lwmutex
#include "path.h"               // getParentPath / getBaseName
#include "string-utilities.h"   // strCopy / truncateUtf8
#include "vfs.h"                // MAX_PATH_LEN
#include "dbg.h"

#include <cell/sysmodule.h>
#include <sysutil/sysutil_common.h>   // cellSysutilCheckCallback
#include <sysutil/sysutil_video_export.h>
#include <sysutil/sysutil_gamecontent.h>

#define MAX_PENDING    8
#define TITLE_MAX      64     // CELL_VIDEO_EXPORT_UTIL_VIDEO_TITLE_MAX_LENGTH
#define ALBUM_TITLE    "Yo! Player Downloads"   // the album the XMB groups these under
#define SHUTDOWN_WAIT_MS 15000   // cap on how long exit waits for a registration to commit
#define SHUTDOWN_POLL_MS 20
#define FALLBACK_DIR   "/dev_hdd0/tmp/yo-player/downloads"   // exports will fail from here; downloads still work

typedef enum { EXPORT_IDLE, EXPORT_INITIALIZING, EXPORT_REGISTERING, EXPORT_FINALIZING } ExportState;

typedef struct { char path[MAX_PATH_LEN]; char title[TITLE_MAX]; } PendingExport;

static struct {
   sys_lwmutex_t mutex;         // guards pending[] and count
   PendingExport pending[MAX_PENDING];
   int           count;

   ExportState   state;
   volatile int  finished;      // the sysutil callback fired
   volatile int  result;        // what it reported
   char          activeName[MAX_PATH_LEN];   // file currently with the system
   char          activeTitle[TITLE_MAX];     // its title: the export reads this after the call returns
   char          stagingDir[MAX_PATH_LEN];   // the directory the export will accept files from
   int           ready;         // the module loaded, so exporting is possible
} exporter;

// the sysutil pump calls this on the same thread as updateVideoExport, so a plain flag is enough
static void onExportFinished(int result, void *userdata)
{
   (void)userdata;
   exporter.result = result;
   exporter.finished = 1;
}

static const char *getExportErrorName(int result)
{
   switch ((unsigned)result) {
      case CELL_VIDEO_EXPORT_UTIL_RET_OK:            return "ok";
      case CELL_VIDEO_EXPORT_UTIL_RET_CANCEL:        return "cancelled";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_BUSY:        return "busy";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_INTERNAL:    return "internal";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM:       return "bad parameter";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_ACCESS_ERROR:return "hdd access";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_DB_INTERNAL: return "database internal";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_DB_REGIST:   return "database register";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_SET_META:    return "set metadata";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_FLUSH_META:  return "flush metadata";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_MOVE:        return "move";
      case CELL_VIDEO_EXPORT_UTIL_ERROR_INITIALIZE:  return "not initialized";
      default:                                       return "unknown";
   }
}

void queueVideoExport(const char *path, const char *title)
{
   if (!exporter.ready || !path || !path[0]) return;

   lock(&exporter.mutex);
   int full = exporter.count >= MAX_PENDING;
   if (!full) {
      PendingExport *item = &exporter.pending[exporter.count++];
      strCopy(item->path, sizeof item->path, path);
      strCopy(item->title, sizeof item->title, title && title[0] ? title : getBaseName(path));
      truncateUtf8(item->title, TITLE_MAX - 1);
   }
   unlock(&exporter.mutex);

   if (full) logWarn("[export] queue full, %s not registered with the xmb\n", getBaseName(path));
}

// hand the head of the queue to the system: it moves the file out of our folder and into the database
static void registerNextFile(void)
{
   char directory[MAX_PATH_LEN];

   lock(&exporter.mutex);
   getParentPath(exporter.pending[0].path, directory, sizeof directory);
   strCopy(exporter.activeName, sizeof exporter.activeName, getBaseName(exporter.pending[0].path));
   strCopy(exporter.activeTitle, sizeof exporter.activeTitle, exporter.pending[0].title);
   unlock(&exporter.mutex);

   exporter.finished = 0;
   logInfo("[export] registering dir='%s' file='%s' title='%s'\n", directory, exporter.activeName, exporter.activeTitle);

   // the call returns before the export finishes, so everything it points at has to outlive this function
   CellVideoExportSetParam param;
   param.title        = exporter.activeTitle;
   param.game_title   = (char *)ALBUM_TITLE;
   param.game_comment = NULL;
   param.editable     = 1;
   param.reserved2    = NULL;

   int rc = cellVideoExportFromFile(directory, exporter.activeName, &param, onExportFinished, NULL);
   if (rc < 0) {
      logError("[export] %s rejected, rc=0x%x (%s)\n", exporter.activeName, rc, getExportErrorName(rc));
      exporter.finished = 1;
      exporter.result = rc;
   }
   exporter.state = EXPORT_REGISTERING;
}

static void dropQueueHead(void)
{
   lock(&exporter.mutex);
   for (int i = 1; i < exporter.count; i++) exporter.pending[i - 1] = exporter.pending[i];
   if (exporter.count > 0) exporter.count--;
   unlock(&exporter.mutex);
}

static int getQueueCount(void)
{
   lock(&exporter.mutex);
   int count = exporter.count;
   unlock(&exporter.mutex);
   return count;
}

// one call per state, in order: initialize once, register every queued file, finalize when the queue empties
void updateVideoExport(void)
{
   if (!exporter.ready) return;

   if (exporter.state == EXPORT_IDLE) {
      if (getQueueCount() == 0) return;
      exporter.finished = 0;
      int rc = cellVideoExportInitialize2(CELL_VIDEO_EXPORT_UTIL_VERSION_CURRENT, onExportFinished, NULL);
      if (rc < 0) {
         logError("[export] initialize failed, rc=0x%x (%s), dropping %d file(s)\n", rc, getExportErrorName(rc), getQueueCount());
         lock(&exporter.mutex);
         exporter.count = 0;
         unlock(&exporter.mutex);
         return;
      }
      exporter.state = EXPORT_INITIALIZING;
      return;
   }

   if (!exporter.finished) return;
   exporter.finished = 0;
   int result = exporter.result;

   switch (exporter.state) {
      case EXPORT_INITIALIZING:
         if (result != CELL_VIDEO_EXPORT_UTIL_RET_OK) {
            logError("[export] initialize failed: %s (0x%x)\n", getExportErrorName(result), result);
            exporter.state = EXPORT_FINALIZING;
            cellVideoExportFinalize(onExportFinished, NULL);
            break;
         }
         registerNextFile();
         break;

      case EXPORT_REGISTERING:
         if (result == CELL_VIDEO_EXPORT_UTIL_RET_OK) logInfo("[export] %s registered with the xmb\n", exporter.activeName);
         else logError("[export] %s failed: %s (0x%x)\n", exporter.activeName, getExportErrorName(result), result);
         dropQueueHead();
         if (getQueueCount() > 0) registerNextFile();
         else {
            exporter.state = EXPORT_FINALIZING;
            cellVideoExportFinalize(onExportFinished, NULL);
         }
         break;

      case EXPORT_FINALIZING:
      default:
         exporter.state = EXPORT_IDLE;
         break;
   }
}

// the export only accepts a directory the game content utility handed out, so downloads have to be
// written there. asking for it is also what tells the system this process owns that directory.
static void resolveStagingDir(void)
{
   strCopy(exporter.stagingDir, sizeof exporter.stagingDir, FALLBACK_DIR);

   int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_GAME);
   if (rc != CELL_OK) { logError("[export] game content module failed to load, rc=0x%x\n", rc); return; }

   unsigned int type = 0, attributes = 0;
   CellGameContentSize size;
   char dirName[CELL_GAME_DIRNAME_SIZE + 1] = { 0 };
   logInfo("[export] calling cellGameBootCheck\n");
   rc = cellGameBootCheck(&type, &attributes, &size, dirName);
   if (rc != CELL_OK) { logError("[export] boot check failed, rc=0x%x\n", rc); return; }
   logInfo("[export] boot check ok, type=%u attributes=0x%x dir='%s'\n", type, attributes, dirName);

   char contentInfoPath[CELL_GAME_PATH_MAX] = { 0 };
   char usrdirPath[CELL_GAME_PATH_MAX] = { 0 };
   logInfo("[export] calling cellGameContentPermit\n");
   rc = cellGameContentPermit(contentInfoPath, usrdirPath);
   if (rc != CELL_OK || !usrdirPath[0]) { logError("[export] content permit failed, rc=0x%x\n", rc); return; }

   strCopy(exporter.stagingDir, sizeof exporter.stagingDir, usrdirPath);
}

void initVideoExport(void)
{
   createLock(&exporter.mutex);
   resolveStagingDir();
   makeDir(exporter.stagingDir);

   if (cellSysmoduleLoadModule(CELL_SYSMODULE_VIDEO_EXPORT) != CELL_OK) {
      logError("[export] video export module missing, downloads will not reach the xmb\n");
      return;
   }
   exporter.ready = 1;
   logInfo("[export] ready, staging in %s\n", exporter.stagingDir);
}

const char *getVideoStagingDir(void) { return exporter.stagingDir; }

// finishing matters: the system writes its database entry as part of the export, and finalize is what
// flushes that metadata, so tearing the module down early loses the registration on the next XMB restart.
static void waitForExportToFinish(void)
{
   for (int waited = 0; waited < SHUTDOWN_WAIT_MS; waited += SHUTDOWN_POLL_MS) {
      if (exporter.state == EXPORT_IDLE && getQueueCount() == 0) return;
      cellSysutilCheckCallback();
      updateVideoExport();
      sleepMs(SHUTDOWN_POLL_MS);
   }
   logWarn("[export] still busy after %d ms, the last registration may not stick\n", SHUTDOWN_WAIT_MS);
}

void shutdownVideoExport(void)
{
   if (exporter.ready) {
      waitForExportToFinish();
      cellSysmoduleUnloadModule(CELL_SYSMODULE_VIDEO_EXPORT);
   }
   exporter.ready = 0;
   destroyLock(&exporter.mutex);
}
