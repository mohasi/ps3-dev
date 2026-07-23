#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sys_time.h>
#include <cell/rtc.h>

#include "metrics-log.h"
#include "console-model.h"
#include "settings.h"
#include "vfs.h"
#include "dbg.h"

#define RUN_DIR "/dev_hdd0/tmp/thermal-bench"
#define SAMPLE_INTERVAL_SECONDS 2

// 4096 samples at one every two seconds is 2 h 16 m of run; past that the oldest
// are dropped so the newest always survive.
#define MAX_SAMPLES 4096

static VfsFile runFile;
static int runOpen;
static int writeFailed;   // a write or flush was refused, so the file is incomplete
static char runPath[256];

static RunSample samples[MAX_SAMPLES];
static int sampleCount;
static RunSample baseline[MAX_SAMPLES];
static int baselineCount;

static RunSummary summary;
static uint64_t startUs;
static int lastSampleSecond;
static int startFanPercent;

int isMetricsLogRecording(void) { return runOpen && !writeFailed; }

const RunSummary *getRunSummary(void) { return &summary; }

RunSamples getRunSamples(void)      { RunSamples run = { samples, sampleCount }; return run; }
RunSamples getBaselineSamples(void) { RunSamples run = { baseline, baselineCount }; return run; }

int getRunElapsedSeconds(void) { return (int)((sys_time_get_system_time() - startUs) / 1000000); }

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

static int loadPreviousRun(void);

void startMetricsLog(const Sensors *initial, const LoadState *load)
{
   // start the run's clock and its bookkeeping
   startUs = sys_time_get_system_time();
   sampleCount = 0;
   lastSampleSecond = -SAMPLE_INTERVAL_SECONDS;
   startFanPercent = initial->fanPercent;

   RunSummary blank = { 0, 0, 0, -1, 0 };
   summary = blank;

   // open the file, named after the date and time
   if (makeDir(RUN_DIR) != 0) logWarn("[bench] could not create %s\n", RUN_DIR);

   CellRtcDateTime now;
   cellRtcGetCurrentClockLocalTime(&now);
   snprintf(runPath, sizeof runPath, "%s/%04d%02d%02d-%02d%02d%02d.csv",
            RUN_DIR, now.year, now.month, now.day, now.hour, now.minute, now.second);

   if (openFs(runPath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &runFile) != 0)
   {
      logError("[bench] could not open run file %s\n", runPath);
      runOpen = 0;
   }
   else
   {
      runOpen = 1;
      writeFailed = 0;
   }

   // header: everything needed to tell this run apart from another
   char header[512];
   snprintf(header, sizeof header,
            "# thermal-bench run\n"
            "# date: %04d-%02d-%02d %02d:%02d:%02d\n"
            "# console: %s, safety cutoff %d C\n"
            "# rsx clocks: core %d MHz, memory %d MHz\n"
            "# fan at start: %s\n"
            "# load at start: cpu %s, gpu %s (levels %d/%d/%d)\n"
            "elapsedSeconds,cpuC,cpuTenths,rsxC,rsxTenths,fanPercent,"
            "cpuLevel,spuThreads,gpuLevel,frameMs,coreMhz,memMhz\n",
            now.year, now.month, now.day, now.hour, now.minute, now.second,
            getConsoleModelSummary(), getSafetyCutoffCelsius(),
            initial->coreClockMhz, initial->memoryClockMhz,
            initial->fanReadable ? getFanModeText(initial->fanMode) : "unreadable",
            getLoadLevelName(load->cpuLevel), getLoadLevelName(load->gpuLevel),
            load->cpuLevel, load->spuThreadCount, load->gpuLevel);
   writeLine(header);

   logInfo("[bench] recording run to %s\n", runPath);

   baselineCount = loadPreviousRun();
}

static void appendRow(int elapsedSeconds, const Sensors *sensors, int frameMs, const LoadState *load)
{
   char row[256];
   snprintf(row, sizeof row, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            elapsedSeconds, sensors->cpuTenthsC / 10, sensors->cpuTenthsC % 10,
            sensors->rsxTenthsC / 10, sensors->rsxTenthsC % 10, sensors->fanPercent,
            load->cpuLevel, load->spuThreadCount, load->gpuLevel, frameMs,
            sensors->coreClockMhz, sensors->memoryClockMhz);
   writeLine(row);
}

void recordMetricsSample(const Sensors *sensors, int frameMs, const LoadState *load)
{
   if (!sensors->temperatureReadable) return;   // a refused reading is not a 0 C reading

   int seconds = getRunElapsedSeconds();
   if (seconds - lastSampleSecond < SAMPLE_INTERVAL_SECONDS) return;
   lastSampleSecond = seconds;

   // keep the newest samples when the ring is full
   RunSample point = { (uint32_t)seconds, (uint16_t)sensors->cpuTenthsC,
                       (uint16_t)sensors->rsxTenthsC, (uint8_t)sensors->fanPercent };
   if (sampleCount == MAX_SAMPLES)
   {
      memmove(samples, samples + 1, sizeof(RunSample) * (MAX_SAMPLES - 1));
      sampleCount--;
   }
   samples[sampleCount++] = point;

   appendRow(seconds, sensors, frameMs, load);

   // running peaks, shown live beside the current values and saved on the way out
   if (sensors->cpuTenthsC > summary.peakCpuTenthsC) summary.peakCpuTenthsC = sensors->cpuTenthsC;
   if (sensors->rsxTenthsC > summary.peakRsxTenthsC) summary.peakRsxTenthsC = sensors->rsxTenthsC;
   if (sensors->fanPercent > summary.peakFanPercent) summary.peakFanPercent = sensors->fanPercent;
   if (summary.firstFanStepSeconds < 0 && sensors->fanReadable && sensors->fanPercent > startFanPercent)
      summary.firstFanStepSeconds = seconds;
   summary.durationSeconds = seconds;
}

void finishMetricsLog(void)
{
   if (!runOpen) return;
   summary.durationSeconds = getRunElapsedSeconds();

   char firstStep[24];
   if (summary.firstFanStepSeconds >= 0) snprintf(firstStep, sizeof firstStep, "%d s", summary.firstFanStepSeconds);
   else                                  snprintf(firstStep, sizeof firstStep, "never");

   char footer[512];
   snprintf(footer, sizeof footer,
            "# summary\n"
            "# duration: %d s\n"
            "# peak cpu: %d.%d C\n"
            "# peak rsx: %d.%d C\n"
            "# peak fan: %d%%\n"
            "# first fan step-up: %s\n",
            summary.durationSeconds, summary.peakCpuTenthsC / 10, summary.peakCpuTenthsC % 10,
            summary.peakRsxTenthsC / 10, summary.peakRsxTenthsC % 10, summary.peakFanPercent, firstStep);
   writeLine(footer);

   closeFs(&runFile);
   runOpen = 0;
   logInfo("[bench] run saved: peak cpu %d.%d C, rsx %d.%d C, fan %d%%\n",
           summary.peakCpuTenthsC / 10, summary.peakCpuTenthsC % 10,
           summary.peakRsxTenthsC / 10, summary.peakRsxTenthsC % 10, summary.peakFanPercent);
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
#define SAMPLE_FIELD_COUNT  6      // elapsed, cpuC, cpuTenths, rsxC, rsxTenths, fanPercent
#define MAX_ELAPSED_SECONDS 604800   // a week
#define MAX_WHOLE_CELSIUS   150
#define MAX_TENTHS          9
#define MAX_FAN_PERCENT     100
#define CSV_HEADER_PREFIX   "elapsed"

static int isInRange(long value, long low, long high) { return value >= low && value <= high; }

// one csv row -> one sample. comment, header and corrupt lines return 0.
static int parseSampleRow(const char *row, RunSample *out)
{
   if (row[0] == '#' || row[0] == 0 || strncmp(row, CSV_HEADER_PREFIX, sizeof CSV_HEADER_PREFIX - 1) == 0) return 0;

   long field[SAMPLE_FIELD_COUNT] = { 0 };
   const char *cursor = row;
   for (int index = 0; index < SAMPLE_FIELD_COUNT; index++)
   {
      if (*cursor < '0' || *cursor > '9') return 0;
      field[index] = strtol(cursor, (char **)&cursor, 10);
      if (index == SAMPLE_FIELD_COUNT - 1) break;
      if (*cursor != ',') return 0;
      cursor++;
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

static int loadPreviousRun(void)
{
   char path[256];
   if (!findPreviousRunPath(path, sizeof path)) return 0;

   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return 0;

   char chunk[2048];
   char row[192];
   int rowLength = 0, count = 0;
   int64_t read;

   while (count < MAX_SAMPLES && (read = readFs(&file, chunk, sizeof chunk)) > 0)
      for (int64_t byte = 0; byte < read && count < MAX_SAMPLES; byte++)
      {
         if (chunk[byte] != '\n')
         {
            if (rowLength < (int)sizeof row - 1) row[rowLength++] = chunk[byte];
            continue;
         }
         row[rowLength] = 0;
         rowLength = 0;
         if (parseSampleRow(row, &baseline[count])) count++;
      }

   closeFs(&file);
   logInfo("[bench] baseline run %s: %d samples\n", path, count);
   return count;
}
