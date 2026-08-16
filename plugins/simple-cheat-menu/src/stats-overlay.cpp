#include "stats-overlay.h"
#include "paf-widget.h"
#include "string-utilities.h"   // appendStr / intToDec: libc-free formatting (no printf in a vsh prx)

#include <stdint.h>
#include <sys/sys_time.h>   // sys_time_get_system_time: microsecond frame timing (liblv2_stub)

// step-logging shims, defined in overlay-bridge.c (C): this TU must not include the C-only dbg.h.
// each call flushes a line to disk before the next paf call, so a hard lock leaves the last step
// reached on disk. logged on state changes only — a per-frame line would flood and slow the frame.
extern "C" void overlayLog(const char *msg);
extern "C" void overlayLogHex(const char *msg, unsigned int value);

// clocks and temperatures, shimmed through overlay-bridge.c so this TU stays clear of the C-only
// syscall headers. Temperatures come back in tenths of a degree, a clock as 0 if it cannot be read.
extern "C" void readStatsSensors(int *coreMhz, int *memoryMhz, int *cpuTenths, int *rsxTenths);

// the settings file, handled in cheat-sync.c alongside the sync mode that shares it
extern "C" void loadStatsSettingsFromFile(int *enabled, int *showGraph, int *showClocks,
                                          int *showTemps, int *topRight);
extern "C" void saveStatsSettings(int enabled, int showGraph, int showClocks, int showTemps, int topRight);


// Widget positions and every size here are in draw surface PIXELS, with the origin at the centre,
// so a corner is at half the real surface size — 640x360 on a 720p output, 960x540 on a 1080p one.
// Reading the real size is why this lands in the corner on any display.
//
// Nothing is scaled by the display size. Text height is a pixel count that PAF does not scale
// either, so scaling the graph but not the text made the block fall out of proportion when the
// console switched resolution: the same numbers everywhere keeps them level. The whole block
// simply covers more of a 720p screen than a 1080p one, exactly as the text does.
#define FALLBACK_WIDTH  1280.0f   // if the surface size cannot be read
#define FALLBACK_HEIGHT  720.0f
#define BOX_MARGIN_PX     10.0f   // between the screen edge and the panel
#define FPS_TEXT_HEIGHT   20.0f
#define ROW_TEXT_HEIGHT   15.0f   // the clock and temperature lines
#define ROW_GAP            5.0f   // between the graph and a row, and between rows

// The frame time graph sits under the readout. One bar per frame, so 30 bars is half a second at
// 60 fps, and longer in the slow games this is really for. Every bar is redrawn every frame, so
// the count is a direct cost on the frame it is measuring.
#define BAR_COUNT          30
#define GRAPH_TOP_MARGIN_PX 5.0f    // real pixels of clear space above the graph's own panel
#define GRAPH_HEIGHT       44.0f
#define GRAPH_WIDTH       210.0f    // pixels, which sits level with the readout line at its usual length
#define BAR_GAP             2.0f    // pixels between bars
#define BAR_MIN_HEIGHT      1.0f    // so a very fast frame still shows something
#define BACKDROP_PADDING    9.0f    // dark ground extends this far past the contents on every side
#define BACKDROP_ALPHA      0.45f   // dark enough to read the text against a bright scene
#define GRAPH_PANEL_ALPHA   0.40f   // the graph's own darker ground, on top of the main one
#define GRAPH_PANEL_PADDING 3.0f    // how far it extends past the bars

// A bar's full height is a frame that took this long. Deliberately a FIXED scale: an axis that
// rescales itself to what it is showing would hide the very difference you are trying to see
// between two clock settings. 66667us is 15 fps, chosen because the games worth overclocking for
// are the ones running below 30, and they need headroom above that rather than below it. 30 fps
// lands at half height and 20 fps at three quarters.
#define FRAME_TIME_CEILING_US  66667.0f

// A rolling window, not the whole session: the average has to move when the clocks move, or it
// cannot show you what an overclock did. One second settles almost immediately after a change
// while still smoothing out single bad frames. The window's frame times sum to its own duration,
// so trimming by that sum is what bounds it in time rather than in frames.
#define AVERAGE_WINDOW_US   1000000
#define AVERAGE_RING_SIZE   128      // a second needs 60 at 60 fps; far fewer in a slow game

// The graph scrolls: oldest on the left, newest on the right, everything sliding left one slot
// per frame. The frame times sit in a ring and only the mapping from screen position to ring slot
// moves, so nothing is copied — but every bar still has to be redrawn, because every bar now
// shows the value its right hand neighbour showed a frame ago. A bar whose value has not changed
// is skipped, which costs nothing on a steady frame rate and everything on a ragged one.

// repaint rate. paf re-measures and re-lays-out the text on every SetText, so repainting at the
// frame rate is both wasteful and unreadable. 4 Hz is the reference's pace and reads steadily.
#define REPAINT_INTERVAL_US  250000

// the name the widget is registered under, so it can be found among its parent's children.
// Unique enough not to collide with the firmware's own, and within PAF_WIDGET_NAME_MAX.
#define FPS_WIDGET_NAME  "SimpleStatsFps"

namespace {

// What the counter shows, driven by the menu's Stats Counter tab and saved to settings.txt.
// A change sets settingsChanged, and the next frame tears the widgets down and builds the new
// set, because which rows exist decides the whole layout.
struct StatsSettings {
   int enabled;
   int showGraph;
   int showClocks;
   int showTemps;
   int topRight;
};

StatsSettings settings = { 1, 1, 1, 1, 0 };
volatile int  settingsChanged = 0;

PafWidget *fpsText = 0;             // also the sentinel: its attachment stands for all of them
PafWidget *clockText = 0;
PafWidget *tempText = 0;
PafWidget *backdrop = 0;
PafWidget *graphPanel = 0;

// Sensor readings, written by the worker thread and read by the drawing one. Clocks and
// temperatures are syscalls and a hypervisor peek; none of that belongs on the frame thread, and
// they change slowly enough that a couple of reads a second is plenty.
volatile int sensorCoreMhz = 0;
volatile int sensorMemoryMhz = 0;
volatile int sensorCpuTenths = 0;
volatile int sensorRsxTenths = 0;
int lastShownCoreMhz = -1, lastShownMemoryMhz = -1;   // so the rows are only rewritten on a change
int lastShownCpuTenths = -1, lastShownRsxTenths = -1;
PafWidget *bars[BAR_COUNT] = { 0 };
PafWidget *averageLine = 0;

// the last BAR_COUNT frame times, oldest first. barOldest is the ring slot the leftmost bar
// shows, so advancing it by one is the whole scroll. lastDrawnUs is what each bar is currently
// showing, so a bar that would not change is left alone.
uint32_t barValues[BAR_COUNT] = { 0 };
uint32_t lastDrawnUs[BAR_COUNT] = { 0 };
int      barOldest = 0;

// the last few seconds of frame times, oldest to newest, with their running total. This is the
// number to compare one set of clocks against another: the graph itself moves far too fast to
// read an average off, and a whole-session average would barely budge when the clocks change.
uint32_t frameRing[AVERAGE_RING_SIZE] = { 0 };
int      ringNext = 0;    // where the next frame goes
int      ringCount = 0;
uint64_t ringTotalUs = 0;
uint64_t lastFrameStampUs = 0;

// width of the readout line in pixels, measured from the widget once PAF has laid it out, which it
// has not done at the moment the widget is created. Only the panel behind everything is sized from
// it, so the text is never clipped. 0 until measured; measureAttempts gives up after a few tries
// rather than asking forever.
float measuredReadoutWidth = 0.0f;
int   measureAttempts = 0;
#define MEASURE_ATTEMPT_LIMIT  8

// why the widget is currently absent, so the log records each transition once instead of
// every frame. NONE means it is up.
enum AbsentReason { ABSENT_NONE, ABSENT_GAME, ABSENT_NO_PARENT, ABSENT_DETACHED, ABSENT_DISABLED, ABSENT_SETTINGS };
AbsentReason absentReason = ABSENT_GAME;

// a game starting or ending is when vsh tears down page_notification and everything under it.
// We step out of that entirely: on either transition we drop our pointers untouched and then
// touch nothing at all — not even the parent lookup — until the teardown is long finished.
// 120 frames is two seconds at 60 fps.
#define SETTLE_FRAMES  120
volatile int gameChanged = 1;   // pad thread raises it, frame thread consumes it; 1 = settle at startup
volatile int gameRunning = 0;   // the counter is for measuring games, so it stays down on the XMB
int          settleFrames = SETTLE_FRAMES;

// frame timing, all on the frame thread. frames counted since the last repaint, divided by the
// elapsed time at repaint. sys_time_get_system_time is microseconds.
uint64_t windowStartUs = 0;
int      framesInWindow = 0;


// Put the counter down. Every widget is a child of the same page and was made together, so the
// named one answers for all of them: still attached means the page is alive and they can be
// destroyed properly; detached means the page is going and they must only be forgotten, because
// touching a widget the firmware has already freed is what locks the console.
void dropWidget(AbsentReason reason, const char *why)
{
   if (absentReason == reason && !fpsText) return;   // already down for this reason
   absentReason = reason;

   if (fpsText && isPafWidgetAttached(fpsText, FPS_WIDGET_NAME)) {
      for (int i = 0; i < BAR_COUNT; i++) destroyPafWidget(bars[i]);
      destroyPafWidget(averageLine);
      destroyPafWidget(graphPanel);
      destroyPafWidget(clockText);
      destroyPafWidget(tempText);
      destroyPafWidget(backdrop);
      destroyPafWidget(fpsText);
   }

   for (int i = 0; i < BAR_COUNT; i++) bars[i] = 0;
   averageLine = 0;
   graphPanel = 0;
   clockText = tempText = 0;
   backdrop = 0;
   fpsText = 0;
   lastShownCoreMhz = lastShownMemoryMhz = -1;
   lastShownCpuTenths = lastShownRsxTenths = -1;
   windowStartUs = 0;
   measuredReadoutWidth = 0.0f;   // measured again on the next build, at that display's size
   overlayLog(why);
}

// Where everything sits, in draw surface pixels with the origin at the screen centre. Derived
// once per build from the real surface size, so the same design figures land correctly on a 720p
// or a 1080p output.
// Everything is laid out from the panel's outer edge inward, so the margin you see between the
// screen edge and the box is the one number that sets it.
struct Layout {
   float boxLeft, boxTop;     // outer edge of the panel
   float contentLeft;         // inside the panel's padding: where every row starts
   float readoutY;            // centre of the FPS line
   float clockY, tempY;       // centres of the two sensor rows
   float boxBottom;           // outer edge below the last row
   float contentWidth;        // the widest row, which the panel is sized to
   float graphBottomY;        // baseline the bars grow up from
   float graphHeight;         // full bar height, in pixels
   float graphWidth;          // however wide the whole-pixel bars came out
   float barPitch, barWidth;  // one bar's slot and the drawn part of it
};

// round to whole pixels, never below one
float toWholePixels(float value)
{
   int rounded = (int)(value + 0.5f);
   return (float)(rounded < 1 ? 1 : rounded);
}

Layout getLayout()
{
   float surfaceWidth  = (float)getPafDrawSurfaceWidth();
   float surfaceHeight = (float)getPafDrawSurfaceHeight();
   if (surfaceWidth < 1.0f || surfaceHeight < 1.0f) { surfaceWidth = FALLBACK_WIDTH; surfaceHeight = FALLBACK_HEIGHT; }

   Layout layout;

   float padX = BACKDROP_PADDING;
   float padY = BACKDROP_PADDING;
   layout.boxTop = surfaceHeight * 0.5f - BOX_MARGIN_PX;

   // Rows stack down the panel, and only the ones switched on take any space, so turning a row
   // off closes the gap rather than leaving a hole. The readout is always there.
   float y = layout.boxTop - padY;
   layout.readoutY = y - FPS_TEXT_HEIGHT * 0.5f;
   y = layout.readoutY - FPS_TEXT_HEIGHT * 0.5f;

   // The margin is measured to the graph's own panel, not to the bars, and it is taken inside this
   // branch so it disappears along with the graph when the row is switched off.
   layout.graphHeight = GRAPH_HEIGHT;
   if (settings.showGraph) {
      float panelPadding = GRAPH_PANEL_PADDING;
      y -= GRAPH_TOP_MARGIN_PX + panelPadding;
      layout.graphBottomY = y - layout.graphHeight;
      y = layout.graphBottomY - panelPadding;   // the panel extends below the bars too
   } else {
      layout.graphBottomY = y;
   }

   if (settings.showClocks) {
      y -= ROW_GAP;
      layout.clockY = y - ROW_TEXT_HEIGHT * 0.5f;
      y = layout.clockY - ROW_TEXT_HEIGHT * 0.5f;
   } else {
      layout.clockY = y;
   }

   if (settings.showTemps) {
      y -= ROW_GAP;
      layout.tempY = y - ROW_TEXT_HEIGHT * 0.5f;
      y = layout.tempY - ROW_TEXT_HEIGHT * 0.5f;
   } else {
      layout.tempY = y;
   }

   layout.boxBottom = y - padY;

   // The graph has a fixed width, set to sit level with the readout line at its normal length.
   // It is deliberately NOT derived from the text: the text changes width as its numbers change,
   // and a graph that resized with it would never hold still.
   //
   // A bar slot that is not a whole number of pixels puts alternate bars a pixel out from their
   // neighbours, which reads as bars grouped in twos, so the slot is rounded first.
   layout.barPitch = toWholePixels(GRAPH_WIDTH / BAR_COUNT);
   layout.barWidth = layout.barPitch - BAR_GAP;
   if (layout.barWidth < 1.0f) layout.barWidth = 1.0f;
   layout.graphWidth = settings.showGraph ? layout.barPitch * BAR_COUNT : 0.0f;

   // finally the horizontal placement, which needs the content width, so it comes last. Top right
   // hangs the panel off the right edge; the rows stay left aligned inside it either way.
   float contentWidth = layout.graphWidth > measuredReadoutWidth ? layout.graphWidth : measuredReadoutWidth;
   layout.contentWidth = contentWidth;
   layout.boxLeft = settings.topRight ? surfaceWidth * 0.5f - BOX_MARGIN_PX - contentWidth - padX * 2.0f
                                      : -surfaceWidth * 0.5f + BOX_MARGIN_PX;
   layout.contentLeft = layout.boxLeft + padX;
   return layout;
}

// Put every widget where the layout says. Called at build, and again once the readout's real
// width is known — PAF has not laid the text out at the moment it is created, so asking for its
// width there returns zero and the graph has to be re-placed a frame or two later.
void placeWidgets(const Layout &layout)
{
   for (int i = 0; i < BAR_COUNT; i++) {
      if (!bars[i]) continue;
      setPafWidgetPosition(bars[i], layout.contentLeft + i * layout.barPitch, layout.graphBottomY);
      setPafWidgetSize(bars[i], layout.barWidth, BAR_MIN_HEIGHT);
      lastDrawnUs[i] = 0;   // force a repaint at the new size
   }

   if (averageLine) setPafWidgetSize(averageLine, layout.graphWidth, 1.0f);
   if (fpsText)   setPafWidgetPosition(fpsText,   layout.contentLeft, layout.readoutY);
   if (clockText) setPafWidgetPosition(clockText, layout.contentLeft, layout.clockY);
   if (tempText)  setPafWidgetPosition(tempText,  layout.contentLeft, layout.tempY);

   if (backdrop) {
      float width = layout.contentWidth + BACKDROP_PADDING * 2.0f;
      setPafWidgetSize(backdrop, width, layout.boxTop - layout.boxBottom);
      setPafWidgetPosition(backdrop, layout.boxLeft + width * 0.5f, (layout.boxTop + layout.boxBottom) * 0.5f);
   }

   if (graphPanel) {
      float pad = GRAPH_PANEL_PADDING;
      setPafWidgetSize(graphPanel, layout.graphWidth + pad * 2.0f, layout.graphHeight + pad * 2.0f);
      setPafWidgetPosition(graphPanel, layout.contentLeft - pad + (layout.graphWidth + pad * 2.0f) * 0.5f,
                           layout.graphBottomY + layout.graphHeight * 0.5f);
   }

   // hide what is switched off rather than building a different set of widgets each time
   if (graphPanel) setPafWidgetColor(graphPanel, 0.0f, 0.0f, 0.0f, settings.showGraph ? GRAPH_PANEL_ALPHA : 0.0f);
   if (clockText)  setPafWidgetColor(clockText, 0.945f, 0.961f, 0.976f, settings.showClocks ? 1.0f : 0.0f);
   if (tempText)   setPafWidgetColor(tempText,  0.945f, 0.961f, 0.976f, settings.showTemps  ? 1.0f : 0.0f);
   if (!settings.showGraph)
      for (int i = 0; i < BAR_COUNT; i++) if (bars[i]) setPafWidgetColor(bars[i], 0.0f, 0.0f, 0.0f, 0.0f);
   if (averageLine && !settings.showGraph)
      setPafWidgetColor(averageLine, 0.0f, 0.0f, 0.0f, 0.0f);
}

// paint the bar at screen position `index` to the height a frame of this length deserves. The bar
// is anchored at its bottom edge, so only its height is written — its position was set once, at
// build. That halves the per-frame layout work, which is what the frame rate here is paying for.
void paintBar(int index, uint32_t frameTimeUs, const Layout &layout)
{
   if (!bars[index] || lastDrawnUs[index] == frameTimeUs) return;
   lastDrawnUs[index] = frameTimeUs;

   float fraction = (float)frameTimeUs / FRAME_TIME_CEILING_US;
   if (fraction > 1.0f) fraction = 1.0f;
   float height = fraction * layout.graphHeight;
   if (height < BAR_MIN_HEIGHT) height = BAR_MIN_HEIGHT;

   setPafWidgetSize(bars[index], layout.barWidth, height);

   // colour against the rates that matter for a game slow enough to be worth overclocking: 30 is
   // the target, 20 is playable, below that is where it hurts
   if (frameTimeUs <= 34000)      setPafWidgetColor(bars[index], 0.204f, 0.827f, 0.600f, 1.0f);   // emerald: 30 fps or better
   else if (frameTimeUs <= 50000) setPafWidgetColor(bars[index], 0.984f, 0.749f, 0.141f, 1.0f);   // amber: 20 to 30
   else                           setPafWidgetColor(bars[index], 0.937f, 0.325f, 0.314f, 1.0f);   // red: under 20
}

// Add this frame to the rolling window and drop whatever has fallen out the back of it. The
// stored frame times add up to the window's own length, so trimming until that total is within
// AVERAGE_WINDOW_US is what makes the window a fixed span of time rather than a fixed count.
void addFrameToWindow(uint32_t frameTimeUs)
{
   if (ringCount == AVERAGE_RING_SIZE) {   // full: the slot about to be written is the oldest
      ringTotalUs -= frameRing[ringNext];
      ringCount--;
   }
   frameRing[ringNext] = frameTimeUs;
   ringNext = (ringNext + 1) % AVERAGE_RING_SIZE;
   ringCount++;
   ringTotalUs += frameTimeUs;

   while (ringTotalUs > AVERAGE_WINDOW_US && ringCount > 1) {
      int oldest = (ringNext - ringCount + AVERAGE_RING_SIZE) % AVERAGE_RING_SIZE;
      ringTotalUs -= frameRing[oldest];
      ringCount--;
   }
}

uint64_t getWindowAverageUs()
{
   return ringCount ? ringTotalUs / (uint64_t)ringCount : 0;
}

// append a number given in hundredths, as "16.72"
void appendHundredths(char *out, int cap, int *end, int hundredths)
{
   (void)cap;
   *end += intToDec(hundredths / 100, out + *end);
   out[(*end)++] = '.';
   int remainder = hundredths % 100;
   if (remainder < 10) out[(*end)++] = '0';
   *end += intToDec(remainder, out + *end);
}

// "FPS 59.9 @ ~16.68 ms" — the live rate, then the average frame time over the last second, with
// either half dropped if its row is switched off.
void buildReadoutText(char *out, int cap, int fpsTenths, int averageHundredthsMs)
{
   int end = 0;
   appendStr(out, cap, &end, "FPS ");
   end += intToDec(fpsTenths / 10, out + end);
   out[end++] = '.';
   end += intToDec(fpsTenths % 10, out + end);
   appendStr(out, cap, &end, " @ ~");
   appendHundredths(out, cap, &end, averageHundredthsMs);
   appendStr(out, cap, &end, " ms");
   out[end] = '\0';
}

// append a tenths value as "62.4"
void appendTenths(char *out, int cap, int *end, int tenths)
{
   (void)cap;
   *end += intToDec(tenths / 10, out + *end);
   out[(*end)++] = '.';
   *end += intToDec(tenths % 10, out + *end);
}

// "RSX 500 / 650 MHz" — core then memory. A zero means cfw peek is unavailable.
void buildClockText(char *out, int cap, int coreMhz, int memoryMhz)
{
   int end = 0;
   appendStr(out, cap, &end, "RSX ");
   if (coreMhz <= 0 || memoryMhz <= 0) {
      appendStr(out, cap, &end, "clocks unavailable");
   } else {
      end += intToDec(coreMhz, out + end);
      appendStr(out, cap, &end, " / ");
      end += intToDec(memoryMhz, out + end);
      appendStr(out, cap, &end, " MHz");
   }
   out[end] = '\0';
}

// "CPU 62.4°C   RSX 58.1°C"
void buildTempText(char *out, int cap, int cpuTenths, int rsxTenths)
{
   int end = 0;
   appendStr(out, cap, &end, "CPU ");
   appendTenths(out, cap, &end, cpuTenths);
   appendStr(out, cap, &end, "\xC2\xB0" "C   RSX ");   // U+00B0 degree sign
   appendTenths(out, cap, &end, rsxTenths);
   appendStr(out, cap, &end, "\xC2\xB0" "C");
   out[end] = '\0';
}

}

void notifyStatsGameChanged(int inGame)
{
   gameRunning = inGame;
   gameChanged = 1;
}

const char *getStatsRowLabel(int row)
{
   switch (row) {
      case STATS_ROW_ENABLED:  return "Stats";
      case STATS_ROW_GRAPH:    return "Frame time graph";
      case STATS_ROW_CLOCKS:   return "Clocks";
      case STATS_ROW_TEMPS:    return "Temperatures";
      case STATS_ROW_POSITION: return "Position";
   }
   return "";
}

const char *getStatsRowValue(int row)
{
   switch (row) {
      case STATS_ROW_ENABLED:  return settings.enabled    ? "ON" : "OFF";
      case STATS_ROW_GRAPH:    return settings.showGraph  ? "ON" : "OFF";
      case STATS_ROW_CLOCKS:   return settings.showClocks ? "ON" : "OFF";
      case STATS_ROW_TEMPS:    return settings.showTemps  ? "ON" : "OFF";
      case STATS_ROW_POSITION: return settings.topRight ? "Top Right" : "Top Left";
   }
   return "";
}

int isStatsRowDisabled(int row)
{
   return row != STATS_ROW_ENABLED && !settings.enabled;
}

void toggleStatsRow(int row)
{
   if (isStatsRowDisabled(row)) return;

   switch (row) {
      case STATS_ROW_ENABLED:  settings.enabled    = !settings.enabled;    break;
      case STATS_ROW_GRAPH:    settings.showGraph  = !settings.showGraph;  break;
      case STATS_ROW_CLOCKS:   settings.showClocks = !settings.showClocks; break;
      case STATS_ROW_TEMPS:    settings.showTemps  = !settings.showTemps;  break;
      case STATS_ROW_POSITION: settings.topRight   = !settings.topRight;   break;
      default: return;
   }

   saveStatsSettings(settings.enabled, settings.showGraph, settings.showClocks,
                     settings.showTemps, settings.topRight);
   settingsChanged = 1;   // the frame thread rebuilds; which rows exist decides the whole layout
}

void loadStatsSettings(void)
{
   loadStatsSettingsFromFile(&settings.enabled, &settings.showGraph, &settings.showClocks,
                             &settings.showTemps, &settings.topRight);
   settingsChanged = 1;
}

void pollStatsSensors(void)
{
   int coreMhz, memoryMhz, cpuTenths, rsxTenths;
   readStatsSensors(&coreMhz, &memoryMhz, &cpuTenths, &rsxTenths);
   sensorCoreMhz = coreMhz;
   sensorMemoryMhz = memoryMhz;
   sensorCpuTenths = cpuTenths;
   sensorRsxTenths = rsxTenths;
}

void updateStatsOverlay(void)
{
   // section: stay clear of the teardown. A game starting or ending is when the firmware rebuilds
   // the widget tree, so that transition is handled before anything else can return early and
   // leave it unconsumed. Nothing in here asks the firmware anything.
   if (gameChanged) {
      gameChanged = 0;
      settleFrames = SETTLE_FRAMES;
      dropWidget(ABSENT_GAME, "stats: game came or went - widget down, settling");
   }

   // a settings change rebuilds from scratch, because which rows are on sets the whole layout
   if (settingsChanged) {
      settingsChanged = 0;
      dropWidget(ABSENT_SETTINGS, "stats: settings changed - rebuilding");
   }

   if (!settings.enabled) { dropWidget(ABSENT_DISABLED, "stats: switched off"); return; }
   if (!gameRunning) return;   // already dropped above; the counter is for measuring games

   if (settleFrames > 0) {
      settleFrames--;
      if (settleFrames == 0) overlayLog("stats: settled");
      return;
   }

   // section: is it safe to touch anything? This is the guard order the VshFpsCounter reference
   // uses, and the reason it survives what locked every earlier version of this file. The
   // firmware unregisters a widget from its parent as it tears that parent's page down, so
   // asking the parent whether it still owns this widget is the one thing that reveals a
   // teardown in progress. It is asked before every write, never after.
   void *parent = findPageNotification();
   if (!parent) { dropWidget(ABSENT_NO_PARENT, "stats: no page_notification - widget down"); return; }

   if (fpsText && !isPafWidgetAttached(fpsText, FPS_WIDGET_NAME)) {
      dropWidget(ABSENT_DETACHED, "stats: widget detached - widget down");
      return;
   }

   // section: build. Memory comes from the firmware's allocator so the firmware can legitimately
   // free it with the rest of the page's children. Nothing is painted on the frame it is built.
   if (!fpsText) {
      void *storage = _sys_malloc(PHTEXT_SIZE);
      if (!storage) return;   // no memory, no counter; the menu is unaffected

      Layout layout = getLayout();
      overlayLogHex("stats: building under parent", (unsigned int)(uintptr_t)parent);
      overlayLogHex("stats: draw surface width", getPafDrawSurfaceWidth());

      // Children draw in the order they are made, so the backdrop is made first and everything
      // else lands on top of it. Its size comes later, once the readout has been measured; only
      // the draw order is fixed here.
      void *backdropStorage = _sys_malloc(sizeof(PafWidget));
      if (backdropStorage) {
         backdrop = makePlaneWidget(backdropStorage, parent, 0.0f, 0.0f, 1.0f, 1.0f);
         setPafWidgetColor(backdrop, 0.02f, 0.02f, 0.04f, BACKDROP_ALPHA);
      }

      // the graph's own darker ground, made after the main backdrop so it sits on top of it and
      // under the bars. Nothing belonging to the graph is built when it is switched off: a hidden
      // widget still costs, because PAF walks every child of the page on every frame.
      if (settings.showGraph) {
         void *graphPanelStorage = _sys_malloc(sizeof(PafWidget));
         if (graphPanelStorage) {
            graphPanel = makePlaneWidget(graphPanelStorage, parent, 0.0f, 0.0f, 1.0f, 1.0f);
            setPafWidgetColor(graphPanel, 0.0f, 0.0f, 0.0f, GRAPH_PANEL_ALPHA);
         }
      }

      // built with a full-length sample so its measured width is the widest the line will get,
      // which is what the graph below is sized to. The real text replaces it on the first repaint.
      char sample[48];
      buildReadoutText(sample, sizeof(sample), 599, 1668);   // the widest the line gets for these settings
      fpsText = makeTextWidget(storage, parent, sample, layout.contentLeft, layout.readoutY, FPS_TEXT_HEIGHT, PAF_ALIGN_LEFT);
      setPafWidgetName(fpsText, FPS_WIDGET_NAME);
      setPafWidgetColor(fpsText, 0.945f, 0.961f, 0.976f, 1.0f);   // SLATE_100, the panel's text colour

      void *clockStorage = _sys_malloc(PHTEXT_SIZE);
      if (clockStorage) {
         clockText = makeTextWidget(clockStorage, parent, "", layout.contentLeft, layout.clockY, ROW_TEXT_HEIGHT, PAF_ALIGN_LEFT);
         setPafWidgetColor(clockText, 0.945f, 0.961f, 0.976f, 1.0f);   // SLATE_100, same white as the readout
      }

      void *tempStorage = _sys_malloc(PHTEXT_SIZE);
      if (tempStorage) {
         tempText = makeTextWidget(tempStorage, parent, "", layout.contentLeft, layout.tempY, ROW_TEXT_HEIGHT, PAF_ALIGN_LEFT);
         setPafWidgetColor(tempText, 0.945f, 0.961f, 0.976f, 1.0f);    // SLATE_100
      }

      // bars, then the average line, so the line draws on top of them
      if (settings.showGraph) {
         for (int i = 0; i < BAR_COUNT; i++) {
            void *barStorage = _sys_malloc(sizeof(PafWidget));
            if (!barStorage) break;   // partial graph beats no counter; unbuilt bars stay null
            bars[i] = makePlaneWidget(barStorage, parent, 0.0f, 0.0f, 1.0f, 1.0f);
            setPafWidgetAnchor(bars[i], PAF_ALIGN_LEFT | PAF_ANCHOR_BOTTOM);
            setPafWidgetColor(bars[i], 0.0f, 0.0f, 0.0f, 0.0f);   // invisible until a frame lands in it
         }

         void *lineStorage = _sys_malloc(sizeof(PafWidget));
         if (lineStorage) {
            averageLine = makePlaneWidget(lineStorage, parent, 0.0f, 0.0f, 1.0f, 1.0f);
            setPafWidgetColor(averageLine, 0.0f, 0.0f, 0.0f, 0.0f);   // placed once there is an average
         }
      }

      placeWidgets(layout);
      barOldest = 0;
      ringNext = ringCount = 0;
      ringTotalUs = 0;
      lastFrameStampUs = 0;
      absentReason = ABSENT_NONE;
      windowStartUs = 0;
      measureAttempts = 0;
      overlayLog("stats: widget up");
      return;
   }

   // section: time this frame
   uint64_t now = sys_time_get_system_time();
   if (!windowStartUs) { windowStartUs = now; lastFrameStampUs = now; framesInWindow = 0; return; }

   uint64_t frameTimeUs = now - lastFrameStampUs;
   lastFrameStampUs = now;
   framesInWindow++;
   addFrameToWindow((uint32_t)frameTimeUs);

   Layout layout = getLayout();

   // section: the graph scrolls left by one bar. The new frame overwrites the oldest ring slot
   // and that slot becomes the rightmost bar, so every screen position now shows what its right
   // hand neighbour showed last frame. Skipped entirely when the graph is switched off, or the
   // bars would repaint themselves visible on top of the readout.
   if (settings.showGraph) {
      barValues[barOldest] = (uint32_t)frameTimeUs;
      barOldest = (barOldest + 1) % BAR_COUNT;
      for (int position = 0; position < BAR_COUNT; position++)
         paintBar(position, barValues[(barOldest + position) % BAR_COUNT], layout);
   }

   // section: the readout and the average line, four times a second — text is re-measured and
   // laid out on every write, so doing it per frame would be wasteful and unreadable anyway.
   uint64_t elapsed = now - windowStartUs;
   if (elapsed < REPAINT_INTERVAL_US) return;

   int fpsTenths = (int)((uint64_t)framesInWindow * 10000000ULL / elapsed);
   windowStartUs = now;
   framesInWindow = 0;

   uint64_t averageUs = getWindowAverageUs();

   // the readout's real width is only knowable once PAF has laid it out, so it is asked for here
   // rather than at build, and the graph is re-placed to match the first time an answer arrives.
   if (measuredReadoutWidth < 1.0f && measureAttempts < MEASURE_ATTEMPT_LIMIT) {
      measureAttempts++;
      float width = getPafTextWidth(fpsText);
      if (width > 1.0f) {
         measuredReadoutWidth = width;
         overlayLogHex("stats: readout width", (unsigned int)width);
         Layout measured = getLayout();
         placeWidgets(measured);
         layout = measured;
      }
   }

   char text[48];
   buildReadoutText(text, sizeof(text), fpsTenths, (int)(averageUs / 10));   // us -> hundredths of a ms
   setPafWidgetText(fpsText, text);

   // the sensor rows are only rewritten when their numbers actually move, which is rarely: clocks
   // change when we change them and temperatures drift over minutes.
   if (clockText && (sensorCoreMhz != lastShownCoreMhz || sensorMemoryMhz != lastShownMemoryMhz)) {
      lastShownCoreMhz = sensorCoreMhz;
      lastShownMemoryMhz = sensorMemoryMhz;
      buildClockText(text, sizeof(text), lastShownCoreMhz, lastShownMemoryMhz);
      setPafWidgetText(clockText, text);
   }

   if (tempText && (sensorCpuTenths != lastShownCpuTenths || sensorRsxTenths != lastShownRsxTenths)) {
      lastShownCpuTenths = sensorCpuTenths;
      lastShownRsxTenths = sensorRsxTenths;
      buildTempText(text, sizeof(text), lastShownCpuTenths, lastShownRsxTenths);
      setPafWidgetText(tempText, text);
   }

   if (averageLine && settings.showGraph) {
      float fraction = (float)averageUs / FRAME_TIME_CEILING_US;
      if (fraction > 1.0f) fraction = 1.0f;
      setPafWidgetPosition(averageLine, layout.contentLeft + layout.graphWidth * 0.5f,
                           layout.graphBottomY + fraction * layout.graphHeight);
      setPafWidgetColor(averageLine, 0.945f, 0.961f, 0.976f, 0.85f);   // SLATE_100, sitting over the bars
   }
}
