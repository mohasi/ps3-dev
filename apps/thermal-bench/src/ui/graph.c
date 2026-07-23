#include <stdio.h>

#include "graph.h"
#include "sensors.h"   // display unit, so the axis follows the readouts
#include "gfx.h"
#include "colors.h"

#define COLOR_CPU   GRAPH_COLOR_CPU
#define COLOR_RSX   GRAPH_COLOR_RSX
#define COLOR_FAN   GRAPH_COLOR_FAN
#define COLOR_GRID  0x33FFFFFF
#define COLOR_AXIS  0x66FFFFFF
#define COLOR_TICK  0xB0FFFFFF

// the temperature axis auto-ranges but stays inside this envelope so the plot
// never zooms in so far that noise looks like a spike.
#define TEMP_AXIS_MIN 20
#define TEMP_AXIS_MAX 95
#define TEMP_AXIS_PAD 5

// a frame can only spend so many vertices (getGfxVertexBudget) and drops whatever
// is drawn past that - which used to take the graph and the button footer with it
// on a long run with a baseline behind it. one segment per two pixels bounds the
// six traces at about 30,000 vertices however long the run gets, and two pixels is
// the line's own thickness, so the trace looks the same.
#define MIN_SEGMENT_PIXELS 2

#define TICK_LABEL_GAP 10   // between a tick caption and the axis it labels

static int getPlotTempY(const Graph *graph, int tenthsC)
{
   int span = (graph->tempAxisHigh - graph->tempAxisLow) * 10;
   if (span <= 0) span = 1;
   return graph->y + graph->height - (tenthsC - graph->tempAxisLow * 10) * graph->height / span;
}

// the fan axis is squashed into the lower part of the plot so its trace does not
// sit on top of the temperature traces for minutes at a time. the fan tick labels
// come from the same function, so the axis still reads true.
#define FAN_AXIS_HEIGHT_PERCENT 75

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

// pick a temperature range that contains the visible data with a little padding,
// clamped to the envelope and rounded to whole tens for tidy gridlines.
static void computeTempAxis(Graph *graph, const GraphSample *live, int liveCount, const GraphSample *baseline, int baselineCount)
{
   int low = TEMP_AXIS_MAX, high = TEMP_AXIS_MIN;
   const GraphSample *sets[] = { live, baseline };
   int counts[] = { liveCount, baselineCount };
   int windowStart = graph->windowEnd - graph->windowSeconds;

   for (int set = 0; set < 2; set++)
      for (int index = 0; index < counts[set]; index++)
      {
         const GraphSample *sample = &sets[set][index];
         if ((int)sample->elapsedSeconds < windowStart) continue;
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
#define LIVE_THICKNESS  2
#define GHOST_THICKNESS 2

static uint32_t fade(uint32_t color) { return (color & 0x00FFFFFF) | GHOST_ALPHA; }

// one segment of a ghost line, cut into dashes. the pattern is measured against
// the screen's own x, not against the segment: a segment can be 30 pixels wide on
// a 2-minute window and 2 pixels wide on a 30-minute one, and cutting each segment
// individually made the long windows come out as a solid line.
#define GHOST_PERIOD_PIXELS (GHOST_DASH_PIXELS + GHOST_GAP_PIXELS)

static void drawDashedLine(int x0, int y0, int x1, int y1, uint32_t color)
{
   int spanX = x1 - x0, spanY = y1 - y0;
   if (spanX <= 0) { drawGfxLine(x0, y0, x1, y1, GHOST_THICKNESS, color); return; }   // vertical: nothing to dash along

   for (int x = x0; x < x1; x = (x / GHOST_PERIOD_PIXELS + 1) * GHOST_PERIOD_PIXELS)
   {
      int dashEnd = x / GHOST_PERIOD_PIXELS * GHOST_PERIOD_PIXELS + GHOST_DASH_PIXELS;   // end of the dash x sits in
      if (dashEnd > x1) dashEnd = x1;
      if (dashEnd <= x) continue;   // x is in a gap

      drawGfxLine(x, y0 + spanY * (x - x0) / spanX, dashEnd, y0 + spanY * (dashEnd - x0) / spanX, GHOST_THICKNESS, color);
   }
}

static void drawTrace(int x0, int y0, int x1, int y1, int ghosted, uint32_t color)
{
   if (ghosted) drawDashedLine(x0, y0, x1, y1, color);
   else         drawGfxLine(x0, y0, x1, y1, LIVE_THICKNESS, color);
}

// draw one run's three traces. ghosted marks a past baseline run.
static void drawSeries(const Graph *graph, const GraphSample *samples, int count, int ghosted)
{
   int windowStart = graph->windowEnd - graph->windowSeconds;

   int prevX = 0, prevCpuY = 0, prevRsxY = 0, prevFanY = 0, havePrev = 0;
   for (int index = 0; index < count; index++)
   {
      const GraphSample *sample = &samples[index];
      int seconds = (int)sample->elapsedSeconds;
      if (seconds < windowStart || seconds > graph->windowEnd) { havePrev = 0; continue; }

      int px = getPlotX(graph, seconds);
      // keep the newest point whatever happens, so the live tip is never clipped
      if (havePrev && px - prevX < MIN_SEGMENT_PIXELS && index < count - 1) continue;

      int cpuY = getPlotTempY(graph, sample->cpuTenthsC);
      int rsxY = getPlotTempY(graph, sample->rsxTenthsC);
      int fanY = getPlotFanY(graph, sample->fanPercent);

      if (havePrev)
      {
         drawTrace(prevX, prevCpuY, px, cpuY, ghosted, ghosted ? fade(COLOR_CPU) : COLOR_CPU);
         drawTrace(prevX, prevRsxY, px, rsxY, ghosted, ghosted ? fade(COLOR_RSX) : COLOR_RSX);
         drawTrace(prevX, prevFanY, px, fanY, ghosted, ghosted ? fade(COLOR_FAN) : COLOR_FAN);
      }
      prevX = px; prevCpuY = cpuY; prevRsxY = rsxY; prevFanY = fanY; havePrev = 1;
   }
}

static int getTickCelsius(const Graph *graph, int tick)
{
   return graph->tempAxisLow + (graph->tempAxisHigh - graph->tempAxisLow) * tick / (GRAPH_TEMP_TICKS - 1);
}

void updateGraph(Graph *graph, const GraphSample *live, int liveCount, const GraphSample *baseline, int baselineCount)
{
   graph->windowEnd = liveCount > 0 ? (int)live[liveCount - 1].elapsedSeconds : graph->windowSeconds;
   if (graph->windowEnd < graph->windowSeconds) graph->windowEnd = graph->windowSeconds;

   computeTempAxis(graph, live, liveCount, baseline, baselineCount);

   int halfLine = graph->labelSize / 2;
   char text[16];

   snprintf(text, sizeof text, "Temp %s", getTemperatureUnitText());
   setLabelText(&graph->tempTitle, text);

   // right-aligned against the axis: fahrenheit ticks are three digits wide and a
   // fixed left edge pushed them over the axis line.
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++)
   {
      int celsius = getTickCelsius(graph, tick);
      snprintf(text, sizeof text, "%d", getDisplayTenths(celsius * 10) / 10);
      setLabelText(&graph->tempTicks[tick], text);
      moveLabel(&graph->tempTicks[tick], graph->x - TICK_LABEL_GAP - graph->tempTicks[tick].tt.tex.w,
                getPlotTempY(graph, celsius * 10) - halfLine);
   }
   for (int tick = 0; tick < GRAPH_FAN_TICKS; tick++)
   {
      int percent = 100 * tick / (GRAPH_FAN_TICKS - 1);
      snprintf(text, sizeof text, "%d", percent);
      setLabelText(&graph->fanTicks[tick], text);
      moveLabel(&graph->fanTicks[tick], graph->x + graph->width + TICK_LABEL_GAP, getPlotFanY(graph, percent) - halfLine);
   }
}

void drawGraph(const Graph *graph, const GraphSample *live, int liveCount, const GraphSample *baseline, int baselineCount)
{
   strokeGfxRectangle(graph->x, graph->y, graph->width, graph->height, 1, COLOR_AXIS);
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++)
   {
      int gridY = getPlotTempY(graph, getTickCelsius(graph, tick) * 10);
      drawGfxLine(graph->x, gridY, graph->x + graph->width, gridY, 1, COLOR_GRID);
   }

   if (baseline) drawSeries(graph, baseline, baselineCount, 1);
   drawSeries(graph, live, liveCount, 0);

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

   initLabelRaw(&graph->tempTitle, font, x - 47, y - labelSize * 2, AUTO, AUTO, labelSize, COLOR_CPU, TEXT_NOWRAP, "");
   initLabel(&graph->fanTitle, font, x + width - labelSize * 2, y - labelSize * 2, AUTO, AUTO, labelSize, COLOR_FAN, TEXT_NOWRAP, "Fan %");
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++) initLabel(&graph->tempTicks[tick], font, x, y, AUTO, AUTO, labelSize, COLOR_TICK, TEXT_NOWRAP, "");
   for (int tick = 0; tick < GRAPH_FAN_TICKS; tick++)  initLabel(&graph->fanTicks[tick],  font, x, y, AUTO, AUTO, labelSize, COLOR_TICK, TEXT_NOWRAP, "");
}

void freeGraph(Graph *graph)
{
   freeLabel(&graph->tempTitle);
   freeLabel(&graph->fanTitle);
   for (int tick = 0; tick < GRAPH_TEMP_TICKS; tick++) freeLabel(&graph->tempTicks[tick]);
   for (int tick = 0; tick < GRAPH_FAN_TICKS; tick++)  freeLabel(&graph->fanTicks[tick]);
}
