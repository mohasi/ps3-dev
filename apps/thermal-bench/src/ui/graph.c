#include <stdio.h>

#include "ui/graph.h"
#include "ui/temperature-unit.h"   // the axis follows whatever unit the readouts show
#include "gfx.h"

#define COLOR_GRID  0x33FFFFFF
#define COLOR_AXIS  0x66FFFFFF
#define COLOR_TICK  0xB0FFFFFF

// the temperature axis auto-ranges but stays inside this envelope so the plot
// never zooms in so far that noise looks like a spike.
#define TEMP_AXIS_MIN 20
#define TEMP_AXIS_MAX 95
#define TEMP_AXIS_PAD 5

// the fan axis is squashed into the lower part of the plot so its trace does not
// sit on top of the temperature traces for minutes at a time. the fan tick labels
// come from the same function, so the axis still reads true.
#define FAN_AXIS_HEIGHT_PERCENT 75

// a frame can only spend so many vertices (getGfxVertexBudget) and drops whatever
// is drawn past that - which used to take the graph and the button footer with it
// on a long run with a baseline behind it. one segment per two pixels bounds the
// six traces at about 30,000 vertices however long the run gets, and two pixels is
// the line's own thickness, so the trace looks the same.
#define MIN_SEGMENT_PIXELS 2

#define TICK_LABEL_GAP    10   // between a tick caption and the axis it labels
#define AXIS_TITLE_INSET  47   // how far left of the plot the temperature title starts

static int getPlotTempY(const Graph *graph, int tenthsC)
{
   int span = (graph->tempAxisHigh - graph->tempAxisLow) * 10;
   if (span <= 0) span = 1;

   // a run file is editable over ftp, so a sample can sit outside the envelope;
   // clamping keeps a corrupt one inside the plot instead of across the readouts.
   int y = graph->y + graph->height - (tenthsC - graph->tempAxisLow * 10) * graph->height / span;
   if (y < graph->y) return graph->y;
   if (y > graph->y + graph->height) return graph->y + graph->height;
   return y;
}

static int getPlotFanY(const Graph *graph, int percent)
{
   int fanHeight = graph->height * FAN_AXIS_HEIGHT_PERCENT / 100;
   return graph->y + graph->height - percent * fanHeight / 100;
}

static int getPlotX(const Graph *graph, int elapsedSeconds)
{
   int windowStart = graph->windowEnd - graph->windowSeconds;
   return graph->x + (elapsedSeconds - windowStart) * graph->width / graph->windowSeconds;
}

static int isInWindow(const Graph *graph, uint32_t elapsedSeconds)
{
   int seconds = (int)elapsedSeconds;
   return seconds >= graph->windowEnd - graph->windowSeconds && seconds <= graph->windowEnd;
}

// pick a temperature range that contains the visible data with a little padding,
// clamped to the envelope and rounded to whole tens for tidy gridlines. only
// samples inside the window count: a longer baseline used to stretch the axis
// with data that is not on screen, squashing the live trace.
static void computeTempAxis(Graph *graph, RunSamples live, RunSamples baseline)
{
   int low = TEMP_AXIS_MAX, high = TEMP_AXIS_MIN;
   RunSamples sets[] = { live, baseline };

   for (int set = 0; set < 2; set++)
      for (int index = 0; index < sets[set].count; index++)
      {
         const RunSample *sample = &sets[set].samples[index];
         if (!isInWindow(graph, sample->elapsedSeconds)) continue;

         int hottest = (sample->cpuTenthsC > sample->rsxTenthsC ? sample->cpuTenthsC : sample->rsxTenthsC) / 10;
         int coolest = (sample->cpuTenthsC < sample->rsxTenthsC ? sample->cpuTenthsC : sample->rsxTenthsC) / 10;
         if (coolest < low)  low = coolest;
         if (hottest > high) high = hottest;
      }

   if (low > high) { low = 40; high = 70; }   // no data yet
   low  -= TEMP_AXIS_PAD;
   high += TEMP_AXIS_PAD;
   if (low  < TEMP_AXIS_MIN) low  = TEMP_AXIS_MIN;
   if (high > TEMP_AXIS_MAX) high = TEMP_AXIS_MAX;
   low  = low  / 10 * 10;
   high = (high + 9) / 10 * 10;
   if (high - low < 10) high = low + 10;
   graph->tempAxisLow = low;
   graph->tempAxisHigh = high;
}

// a baseline run is the same line, faded back and dashed so it reads as a ghost
// of the run in front of it and never as live data.
#define GHOST_ALPHA 0x40000000
#define GHOST_DASH_PIXELS 5
#define GHOST_GAP_PIXELS  5
#define GHOST_PERIOD_PIXELS (GHOST_DASH_PIXELS + GHOST_GAP_PIXELS)
#define LIVE_THICKNESS  2
#define GHOST_THICKNESS 2

static uint32_t fade(uint32_t color) { return (color & 0x00FFFFFF) | GHOST_ALPHA; }

// one segment of a ghost line, cut into dashes. the pattern is measured against
// the screen's own x, not against the segment: a segment can be 30 pixels wide on
// a 2-minute window and 2 pixels wide on a 30-minute one, and cutting each segment
// individually made the long windows come out as a solid line.
static void drawDashedLine(int x0, int y0, int x1, int y1, uint32_t color)
{
   int spanX = x1 - x0, spanY = y1 - y0;
   if (spanX <= 0) { drawGfxLine(x0, y0, x1, y1, GHOST_THICKNESS, color); return; }   // vertical: nothing to dash along

   for (int x = x0; x < x1; x = (x / GHOST_PERIOD_PIXELS + 1) * GHOST_PERIOD_PIXELS)
   {
      int dashEnd = x / GHOST_PERIOD_PIXELS * GHOST_PERIOD_PIXELS + GHOST_DASH_PIXELS;   // end of the dash x sits in
      if (dashEnd > x1) dashEnd = x1;
      if (dashEnd <= x) continue;   // x is in a gap

      int dashY = y0 + spanY * (x - x0) / spanX, dashEndY = y0 + spanY * (dashEnd - x0) / spanX;
      drawGfxLine(x, dashY, dashEnd, dashEndY, GHOST_THICKNESS, color);
   }
}

static void drawTrace(int x0, int y0, int x1, int y1, int ghosted, uint32_t color)
{
   if (ghosted) drawDashedLine(x0, y0, x1, y1, color);
   else         drawGfxLine(x0, y0, x1, y1, LIVE_THICKNESS, color);
}

// draw one run's three traces. ghosted marks a past baseline run.
static void drawSeries(const Graph *graph, RunSamples run, int ghosted)
{
   int prevX = 0, prevCpuY = 0, prevRsxY = 0, prevFanY = 0, havePrev = 0;
   for (int index = 0; index < run.count; index++)
   {
      const RunSample *sample = &run.samples[index];
      if (!isInWindow(graph, sample->elapsedSeconds)) { havePrev = 0; continue; }

      int px = getPlotX(graph, (int)sample->elapsedSeconds);
      // keep the newest point whatever happens, so the live tip is never clipped
      if (havePrev && px - prevX < MIN_SEGMENT_PIXELS && index < run.count - 1) continue;

      int cpuY = getPlotTempY(graph, sample->cpuTenthsC);
      int rsxY = getPlotTempY(graph, sample->rsxTenthsC);
      int fanY = getPlotFanY(graph, sample->fanPercent);

      if (havePrev)
      {
         drawTrace(prevX, prevCpuY, px, cpuY, ghosted, ghosted ? fade(GRAPH_COLOR_CPU) : GRAPH_COLOR_CPU);
         drawTrace(prevX, prevRsxY, px, rsxY, ghosted, ghosted ? fade(GRAPH_COLOR_RSX) : GRAPH_COLOR_RSX);
         drawTrace(prevX, prevFanY, px, fanY, ghosted, ghosted ? fade(GRAPH_COLOR_FAN) : GRAPH_COLOR_FAN);
      }
      prevX = px; prevCpuY = cpuY; prevRsxY = rsxY; prevFanY = fanY; havePrev = 1;
   }
}

static int getTickCelsius(const Graph *graph, int tick)
{
   return graph->tempAxisLow + (graph->tempAxisHigh - graph->tempAxisLow) * tick / (GRAPH_TEMP_TICKS - 1);
}

static int getTickFanPercent(int tick) { return 100 * tick / (GRAPH_FAN_TICKS - 1); }

void updateGraph(Graph *graph, RunSamples live, RunSamples baseline)
{
   graph->windowEnd = live.count > 0 ? (int)live.samples[live.count - 1].elapsedSeconds : graph->windowSeconds;
   if (graph->windowEnd < graph->windowSeconds) graph->windowEnd = graph->windowSeconds;

   computeTempAxis(graph, live, baseline);

   char text[16];
   snprintf(text, sizeof text, "Temp %s", getTemperatureUnitText());
   setLabelText(&graph->tempTitle, text);

   // the temperature ticks move with the axis. right-aligned against it, because a
   // fixed left edge pushed three-digit fahrenheit captions over the axis line.
   int halfLine = graph->labelSize / 2;
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++)
   {
      int celsius = getTickCelsius(graph, tick);
      snprintf(text, sizeof text, "%d", getDisplayTenths(celsius * 10) / 10);
      setLabelText(&graph->tempTicks[tick], text);
      moveLabel(&graph->tempTicks[tick], graph->x - TICK_LABEL_GAP - graph->tempTicks[tick].tt.tex.w,
                getPlotTempY(graph, celsius * 10) - halfLine);
   }
}

void drawGraph(const Graph *graph, RunSamples live, RunSamples baseline)
{
   strokeGfxRectangle(graph->x, graph->y, graph->width, graph->height, 1, COLOR_AXIS);
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++)
   {
      int gridY = getPlotTempY(graph, getTickCelsius(graph, tick) * 10);
      drawGfxLine(graph->x, gridY, graph->x + graph->width, gridY, 1, COLOR_GRID);
   }

   drawSeries(graph, baseline, 1);
   drawSeries(graph, live, 0);

   drawLabel(&graph->tempTitle);
   drawLabel(&graph->fanTitle);
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++) drawLabel(&graph->tempTicks[tick]);
   for (int tick = 0; tick < GRAPH_FAN_TICKS; tick++) drawLabel(&graph->fanTicks[tick]);
}

void initGraph(Graph *graph, Font *font, int labelSize, int x, int y, int width, int height, int windowSeconds)
{
   graph->x = x; graph->y = y; graph->width = width; graph->height = height;
   graph->labelSize = labelSize;
   graph->windowSeconds = windowSeconds;
   graph->windowEnd = windowSeconds;
   graph->tempAxisLow = 40; graph->tempAxisHigh = 70;

   // the axis titles sit above the plot, each one over its own tick column
   int titleX = x - AXIS_TITLE_INSET;
   int titleY = y - labelSize * 2;
   initLabelRaw(&graph->tempTitle, font, titleX, titleY, AUTO, AUTO, labelSize, GRAPH_COLOR_CPU, TEXT_NOWRAP, "");
   int fanTitleX = x + width - labelSize * 2;
   initLabel(&graph->fanTitle, font, fanTitleX, titleY, AUTO, AUTO, labelSize, GRAPH_COLOR_FAN, TEXT_NOWRAP, "Fan %");

   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++)
      initLabel(&graph->tempTicks[tick], font, x, y, AUTO, AUTO, labelSize, COLOR_TICK, TEXT_NOWRAP, "");

   // the fan axis never moves - 0 to 100 always - so its captions are placed once
   char text[8];
   int halfLine = labelSize / 2;
   for (int tick = 0; tick < GRAPH_FAN_TICKS; tick++)
   {
      int percent = getTickFanPercent(tick);
      snprintf(text, sizeof text, "%d", percent);
      initLabel(&graph->fanTicks[tick], font, x + width + TICK_LABEL_GAP, getPlotFanY(graph, percent) - halfLine,
                AUTO, AUTO, labelSize, COLOR_TICK, TEXT_NOWRAP, text);
   }
}

void freeGraph(Graph *graph)
{
   freeLabel(&graph->tempTitle);
   freeLabel(&graph->fanTitle);
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++) freeLabel(&graph->tempTicks[tick]);
   for (int tick = 0; tick < GRAPH_FAN_TICKS; tick++)  freeLabel(&graph->fanTicks[tick]);
}
