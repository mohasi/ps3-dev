#pragma once

// graph - dual-axis time plot for a bench run. left axis is temperature, right
// axis is fan duty %, x axis is elapsed time. draws the live run as solid lines
// and, optionally, a past run behind it as faded ghost lines to compare against.

#include "font.h"
#include "colors.h"
#include "ui/label.h"

// series colours: cpu warm, rsx hot, fan cool. shared so the readouts and the
// key can match their traces.
#define GRAPH_COLOR_CPU COLOR_ORANGE_400
#define GRAPH_COLOR_RSX COLOR_ROSE_400
#define GRAPH_COLOR_FAN COLOR_SKY_400

// one point in a run's history. temperatures are in tenths of a degree - whole
// degrees drew a visible staircase. elapsed time is full width: 16 bits wrapped
// after 18 hours and blanked the graph on a long run.
typedef struct GraphSample {
   uint32_t elapsedSeconds;
   uint16_t cpuTenthsC;
   uint16_t rsxTenthsC;
   uint8_t  fanPercent;
} GraphSample;

#define GRAPH_TEMP_TICKS 5
#define GRAPH_FAN_TICKS  5

typedef struct Graph {
   int x, y, width, height;      // plot area, inside the axis labels
   int labelSize;                // text size of the axis labels
   int windowSeconds;            // visible time span, ending at the newest sample
   int windowEnd;                // elapsed time at the right-hand edge, in seconds
   int tempAxisLow, tempAxisHigh;   // whole degrees celsius, recomputed from the visible data
   Label tempTitle, fanTitle;
   Label tempTicks[GRAPH_TEMP_TICKS];
   Label fanTicks[GRAPH_FAN_TICKS];
} Graph;

void initGraph(Graph *graph, Font *font, int labelSize, int x, int y, int width, int height, int windowSeconds);
void freeGraph(Graph *graph);

// re-ranges the axis and rebuilds the tick captions. call from the update path,
// never from the draw path: setting label text re-rasterises it, which drains
// the whole graphics pipeline mid-frame.
void updateGraph(Graph *graph, const GraphSample *live, int liveCount, const GraphSample *baseline, int baselineCount);

// draws grid, axes and the live run. baseline may be NULL; when set it is drawn
// behind the live run as faded lines, aligned on elapsed time from each run's
// start, so the gap between the two is the effect of whatever changed.
void drawGraph(const Graph *graph, const GraphSample *live, int liveCount, const GraphSample *baseline, int baselineCount);
