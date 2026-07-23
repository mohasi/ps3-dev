#pragma once

// metrics-log - the run recorder. it owns the samples a run produces, writes one
// CSV file per run under /dev_hdd0/tmp/thermal-bench/ through vfs, and reads the
// newest previous run back in so it can be drawn behind the live one. flushed
// every sample so a hard lock still leaves the data on disk.
//
// the file's header records the conditions (date, console, clocks, load) so two
// runs can be told apart; a trailing comment block holds the summary.

#include "run-sample.h"
#include "sensors.h"
#include "stress.h"

// peaks are kept in tenths of a degree so they read at the same precision as the
// live values.
typedef struct RunSummary {
   int peakCpuTenthsC, peakRsxTenthsC, peakFanPercent;
   int firstFanStepSeconds;   // when the fan first rose above its starting duty, -1 if never
   int durationSeconds;
} RunSummary;

// opens the file, writes the header and loads the previous run. the file is named
// after the date and time it was started.
void startMetricsLog(const Sensors *initial, const LoadState *load);

// one sensor reading. a sample is recorded only when enough time has passed since
// the last one, so the caller does not have to keep the cadence; a reading the
// console refused is not recorded at all, because a made-up 0 C would come back as
// real data the next time this run is used as a baseline.
void recordMetricsSample(const Sensors *sensors, int frameMs, const LoadState *load);

// writes the summary block and closes the file.
void finishMetricsLog(void);

int getRunElapsedSeconds(void);

// the peaks so far - the screen shows them beside the live values.
const RunSummary *getRunSummary(void);

RunSamples getRunSamples(void);
RunSamples getBaselineSamples(void);   // the previous run; empty when this is the first ever

// 0 once anything has gone wrong with the file - it could not be opened, or a
// write was refused part way through. the screen says so, because a run that
// recorded nothing otherwise looks exactly like one that recorded everything.
int isMetricsLogRecording(void);
