#pragma once

// metrics-log - one CSV file per run under /dev_hdd0/tmp/thermal-bench/, written
// through vfs. flushed every sample so a hard lock still leaves the data on disk.
// the header records the conditions (date, clocks, load) so two runs can be told
// apart; a trailing comment block holds the summary.

#include "sensors.h"
#include "stress.h"
#include "graph.h"

// peaks are kept in tenths of a degree so they read at the same precision as the
// live values.
typedef struct RunSummary {
   int peakCpuTenthsC, peakRsxTenthsC, peakFanPercent;
   int firstFanStepSeconds;   // when the fan first rose above its starting duty, -1 if never
   int durationSeconds;
} RunSummary;

// opens the file and writes the header. the file is named after the date and
// time it was started.
void startMetricsLog(const Sensors *initial, const LoadState *load);

void appendMetricsSample(int elapsedSeconds, const Sensors *sensors, int frameMs, const LoadState *load);

void finishMetricsLog(const RunSummary *summary);

// 0 once anything has gone wrong with the file - it could not be opened, or a
// write was refused part way through. the screen says so, because a run that
// recorded nothing otherwise looks exactly like one that recorded everything.
int isMetricsLogRecording(void);

// reads the newest previous run back in, so it can be drawn behind the live one
// as a ghosted baseline. returns how many samples were loaded (0 if this is the
// first run ever). call after startMetricsLog so the file being written now is
// skipped.
int loadPreviousRun(GraphSample *out, int capacity);
