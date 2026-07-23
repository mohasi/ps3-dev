#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cell/rtc.h>

#include "metrics-log.h"
#include "console-model.h"
#include "settings.h"
#include "vfs.h"
#include "dbg.h"

#define RUN_DIR "/dev_hdd0/tmp/thermal-bench"

static VfsFile runFile;
static int runOpen;
static int writeFailed;   // a write or flush was refused, so the file is incomplete
static char runPath[256];

int isMetricsLogRecording(void) { return runOpen && !writeFailed; }

static void writeLine(const char *text)
{
   if (!isMetricsLogRecording()) return;

   uint64_t length = (uint64_t)strlen(text);
   if (writeFs(&runFile, text, length) != (int64_t)length || fsyncFs(&runFile) != 0)
   {
      logError("[bench] run file write failed - recording stopped (%s)\n", runPath);
      writeFailed = 1;
   }
}

void startMetricsLog(const Sensors *initial, const LoadState *load)
{
   if (makeDir(RUN_DIR) != 0) logWarn("[bench] could not create %s\n", RUN_DIR);

   CellRtcDateTime now;
   cellRtcGetCurrentClockLocalTime(&now);
   snprintf(runPath, sizeof runPath, "%s/%04d%02d%02d-%02d%02d%02d.csv",
            RUN_DIR, now.year, now.month, now.day, now.hour, now.minute, now.second);

   if (openFs(runPath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &runFile) != 0)
   {
      logError("[bench] could not open run file %s\n", runPath);
      runOpen = 0;
      return;
   }
   runOpen = 1;
   writeFailed = 0;

   char header[512];
   snprintf(header, sizeof header,
            "# thermal-bench run\n"
            "# date: %04d-%02d-%02d %02d:%02d:%02d\n"
            "# console: %s, safety cutoff %d C\n"
            "# rsx clocks: core %d MHz, memory %d MHz\n"
            "# fan at start: %s\n"
            "# load at start: cpu %s, gpu %s (levels %d/%d/%d)\n"
            "elapsedSeconds,cpuC,cpuTenths,rsxC,rsxTenths,fanPercent,cpuLevel,spuLevel,gpuLevel,frameMs,coreMhz,memMhz\n",
            now.year, now.month, now.day, now.hour, now.minute, now.second,
            getConsoleModelSummary(), getSafetyCutoffCelsius(),
            initial->coreClockMhz, initial->memoryClockMhz,
            initial->fanReadable ? getFanModeText(initial->fanMode) : "unreadable",
            getLoadLevelName(load->cpuLevel), getLoadLevelName(load->gpuLevel), load->cpuLevel, load->spuLevel, load->gpuLevel);
   writeLine(header);

   logInfo("[bench] recording run to %s\n", runPath);
}

void appendMetricsSample(int elapsedSeconds, const Sensors *sensors, int frameMs, const LoadState *load)
{
   char row[256];
   snprintf(row, sizeof row, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            elapsedSeconds, sensors->cpuTenthsC / 10, sensors->cpuTenthsC % 10, sensors->rsxTenthsC / 10, sensors->rsxTenthsC % 10,
            sensors->fanPercent, load->cpuLevel, load->spuLevel, load->gpuLevel, frameMs,
            sensors->coreClockMhz, sensors->memoryClockMhz);
   writeLine(row);
}

void finishMetricsLog(const RunSummary *summary)
{
   if (!runOpen) return;

   char firstStep[24];
   if (summary->firstFanStepSeconds >= 0) snprintf(firstStep, sizeof firstStep, "%d s", summary->firstFanStepSeconds);
   else                                   snprintf(firstStep, sizeof firstStep, "never");

   char footer[512];
   snprintf(footer, sizeof footer,
            "# summary\n"
            "# duration: %d s\n"
            "# peak cpu: %d.%d C\n"
            "# peak rsx: %d.%d C\n"
            "# peak fan: %d%%\n"
            "# first fan step-up: %s\n",
            summary->durationSeconds, summary->peakCpuTenthsC / 10, summary->peakCpuTenthsC % 10,
            summary->peakRsxTenthsC / 10, summary->peakRsxTenthsC % 10, summary->peakFanPercent, firstStep);
   writeLine(footer);

   closeFs(&runFile);
   runOpen = 0;
   logInfo("[bench] run saved: peak cpu %d.%d C, rsx %d.%d C, fan %d%%\n",
           summary->peakCpuTenthsC / 10, summary->peakCpuTenthsC % 10,
           summary->peakRsxTenthsC / 10, summary->peakRsxTenthsC % 10, summary->peakFanPercent);
}

// the newest run file that is not the one we are writing now. names start with
// the date and time, so "newest" is just the greatest name.
static int findPreviousRunPath(char *out, int capacity)
{
   const char *currentName = strrchr(runPath, '/');
   currentName = currentName ? currentName + 1 : runPath;

   VfsDir dir;
   if (openDir(RUN_DIR, &dir) != 0) return 0;

   char newest[128] = "";
   char name[128];
   while (readDir(&dir, name, sizeof name, NULL) == 1)
   {
      int length = (int)strlen(name);
      if (length < 5 || strcmp(name + length - 4, ".csv") != 0) continue;
      if (strcmp(name, currentName) == 0) continue;
      if (strcmp(name, newest) > 0) snprintf(newest, sizeof newest, "%s", name);
   }
   closeDir(&dir);

   if (newest[0] == 0) return 0;
   snprintf(out, capacity, "%s/%s", RUN_DIR, newest);
   return 1;
}

// a run file is plain text on a disk anyone can reach over ftp, so treat every
// field as untrusted: anything outside what the sensors can physically report
// means the row is corrupt and the whole row is dropped.
#define MAX_ELAPSED_SECONDS 604800   // a week
#define MAX_WHOLE_CELSIUS   150
#define MAX_TENTHS          9
#define MAX_FAN_PERCENT     100

static int isInRange(long value, long low, long high) { return value >= low && value <= high; }

// one csv row -> one graph sample. comment, header and corrupt lines return 0.
static int parseSampleRow(const char *row, GraphSample *out)
{
   if (row[0] == '#' || row[0] == 'e' || row[0] == 0) return 0;

   long field[6] = { 0 };   // elapsed, cpuC, cpuTenths, rsxC, rsxTenths, fanPercent
   const char *cursor = row;
   for (int index = 0; index < 6; index++)
   {
      if (*cursor < '0' || *cursor > '9') return 0;
      field[index] = strtol(cursor, (char **)&cursor, 10);
      if (index < 5)
      {
         if (*cursor != ',') return 0;
         cursor++;
      }
   }

   if (!isInRange(field[0], 0, MAX_ELAPSED_SECONDS)) return 0;
   if (!isInRange(field[1], 0, MAX_WHOLE_CELSIUS) || !isInRange(field[2], 0, MAX_TENTHS)) return 0;
   if (!isInRange(field[3], 0, MAX_WHOLE_CELSIUS) || !isInRange(field[4], 0, MAX_TENTHS)) return 0;
   if (!isInRange(field[5], 0, MAX_FAN_PERCENT)) return 0;

   out->elapsedSeconds = (uint32_t)field[0];
   out->cpuTenthsC     = (uint16_t)(field[1] * 10 + field[2]);
   out->rsxTenthsC     = (uint16_t)(field[3] * 10 + field[4]);
   out->fanPercent     = (uint8_t)field[5];
   return 1;
}

int loadPreviousRun(GraphSample *out, int capacity)
{
   char path[256];
   if (!findPreviousRunPath(path, sizeof path)) return 0;

   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return 0;

   char chunk[2048];
   char row[192];
   int rowLength = 0, count = 0;
   int64_t read;

   while (count < capacity && (read = readFs(&file, chunk, sizeof chunk)) > 0)
      for (int64_t byte = 0; byte < read && count < capacity; byte++)
      {
         if (chunk[byte] != '\n')
         {
            if (rowLength < (int)sizeof row - 1) row[rowLength++] = chunk[byte];
            continue;
         }
         row[rowLength] = 0;
         rowLength = 0;
         if (parseSampleRow(row, &out[count])) count++;
      }

   closeFs(&file);
   logInfo("[bench] baseline run %s: %d samples\n", path, count);
   return count;
}
