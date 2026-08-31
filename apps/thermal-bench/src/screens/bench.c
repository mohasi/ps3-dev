// bench screen - the main view. a full-screen stress canvas is drawn behind a
// semi-transparent scrim; the live readouts, a key and the graph float on top,
// like a furmark overlay. reads the sensors every 500 ms and hands each reading
// to the recorder and to the safety watch.
#include <sys/sys_time.h>
#include <stdio.h>

#include "screens/bench.h"
#include "sensors.h"
#include "console-model.h"
#include "settings.h"
#include "safety.h"
#include "metrics-log.h"
#include "stress.h"
#include "clocks.h"
#include "ui/graph.h"
#include "ui/temperature-unit.h"
#include "app.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "thread.h"

#define READOUT_INTERVAL_US 500000
#define SCRIM_COLOR 0x80000000     // 50% black over the stress canvas

// the two lines under the title and the load are subtitles: smaller, and a touch
// faded so they sit behind the readings they explain.
#define SUBTITLE_COLOR ((COLOR_SLATE_300 & 0x00FFFFFF) | 0xE5000000)   // 90% opacity

// the readouts, in display order. the three that have a graph line take that
// line's colour so a number and its trace read as the same thing.
typedef enum StatRow { STAT_CPU, STAT_RSX, STAT_FAN, STAT_CORE_CLOCK, STAT_MEMORY_CLOCK, STAT_ROW_COUNT } StatRow;

static const char *STAT_NAMES[STAT_ROW_COUNT] = { "CELL", "RSX", "Fan", "RSX Clock", "Mem Clock" };
static const uint32_t STAT_COLORS[STAT_ROW_COUNT] = {
   GRAPH_COLOR_CPU, GRAPH_COLOR_RSX, GRAPH_COLOR_FAN, COLOR_SLATE_300, COLOR_SLATE_300
};

// the three readings that have a graph line, for the key above the graph
#define KEY_ENTRY_COUNT 3
static const StatRow KEY_ROWS[KEY_ENTRY_COUNT] = { STAT_CPU, STAT_RSX, STAT_FAN };

static const int WINDOW_CHOICES[] = { 120, 300, 600, 1800 };
#define WINDOW_CHOICE_COUNT (int)(sizeof WINDOW_CHOICES / sizeof WINDOW_CHOICES[0])

#define KEY_SWATCH_WIDTH   26
#define KEY_SWATCH_GAP      8
#define KEY_ENTRY_GAP      34

// pixel nudges the derived layout cannot express: the header sits a little above
// the stats, and each subtitle a little under the line it belongs to.
#define HEADER_LIFT_PIXELS   30
#define SUBTITLE_GAP_PIXELS   5
#define GRAPH_AXIS_GUTTER    50   // room for the tick captions either side of the plot
#define HINT_ROW_GAP_PIXELS  12   // between the two hint rows

// every position the screen is laid out from, worked out once from the screen size
typedef struct BenchLayout {
   int sideMargin, verticalMargin;
   int textSize, subtitleSize, spacing;
   int headerTop;     // the stats and the notice line
   int titleTop;      // the title and the load line, a little higher
   int subtitleTop;   // the model line and the frame-time line
   int hintsTop;      // the first of the two hint rows, which the graph sits above
} BenchLayout;

static Font font;
static Label title;
static Label modelLabel;
static Label statNames[STAT_ROW_COUNT];
static Label statValues[STAT_ROW_COUNT];
static Label loadLabel;
static Label frameLabel;
static Label noticeLabel;
static Label keyLabels[KEY_ENTRY_COUNT];
static int keySwatchX[KEY_ENTRY_COUNT];
static Graph graph;
static ButtonHints settingsHints;   // clocks, units and the graph window
static ButtonHints loadHints;       // the three load toggles, on their own row
static int timeWindowHintIndex;

static uint64_t lastReadoutUs;
static int framesSinceReadout;
static int frameMs;
static int windowChoice = 1;
static int menuWasOpen;

static Sensors latestSensors;
static BenchLayout layout;

// "cutoff 70 °C" in whatever unit is on screen, for the model line and the notice
static void getCutoffText(char *out, int capacity)
{
   int shown = getDisplayTenths(getSafetyCutoffCelsius() * 10) / 10;
   snprintf(out, capacity, "cutoff %d %s", shown, getTemperatureUnitText());
}

// one line for whatever the user cannot otherwise see: why the load switched
// itself off, that the run is not being recorded, that the fan is not going to
// respond, that a clock change outlives the app.
static void refreshNotice(void)
{
   const LoadState *load = getLoadState();
   int clocksChanged = latestSensors.coreClockMhz != getBootRsxCoreClockMhz()
                    || latestSensors.memoryClockMhz != getBootRsxMemoryClockMhz();
   int loaded = load->cpuLevel != LOAD_OFF || load->gpuLevel != LOAD_OFF;
   int fanIsManual = latestSensors.fanReadable && latestSensors.fanMode == FAN_MODE_MANUAL;

   char cutoffText[LABEL_MAX_TEXT];
   const char *text = "";

   if (hasSafetyCutoffFired() && !latestSensors.temperatureReadable)
      text = "safety cutoff: temperature unreadable - load off, clocks back to boot";
   else if (hasSafetyCutoffFired())
   {
      char cutoff[48];
      getCutoffText(cutoff, sizeof cutoff);
      snprintf(cutoffText, sizeof cutoffText, "safety %s - load off, clocks back to boot", cutoff);
      text = cutoffText;
   }
   else if (!isMetricsLogRecording())
      text = "not recording - this run is not being saved";
   else if (clocksChanged)
      text = "clocks changed - the new clocks stay after exit, until the console reboots";
   else if (loaded && fanIsManual)
      text = "the fan is set manually - it will not step up as the console heats";

   setLabelText(&noticeLabel, text);
   int rightEdge = getGfxScreenWidth() - layout.sideMargin;
   moveLabel(&noticeLabel, rightEdge - noticeLabel.tt.tex.w, layout.headerTop + layout.spacing * 2);
}

// which console this is and the temperature its load switches off at. static text,
// so it is only rebuilt when the displayed unit changes.
static void refreshModelLabel(void)
{
   char cutoffText[LABEL_MAX_TEXT], text[LABEL_MAX_TEXT];
   getCutoffText(cutoffText, sizeof cutoffText);
   snprintf(text, sizeof text, "%s - %s%s", getConsoleModelSummary(), cutoffText,
            isSafetyCutoffFromSettings() ? " (overridden in settings.txt)" : "");
   setLabelText(&modelLabel, text);
}

// one clock readout: the live value with the boot value beside it, because a clock
// change here is global and stays after the app closes.
static void setClockValue(StatRow row, int liveMhz, int bootMhz)
{
   char text[LABEL_MAX_TEXT];
   if (liveMhz > 0) snprintf(text, sizeof text, "%d MHz  (boot %d)", liveMhz, bootMhz);
   else             snprintf(text, sizeof text, "unreadable");
   setLabelText(&statValues[row], text);
}

static void refreshReadouts(void)
{
   char text[LABEL_MAX_TEXT];
   const RunSummary *summary = getRunSummary();

   // temperatures: now / peak so far
   if (latestSensors.temperatureReadable)
   {
      const char *unit = getTemperatureUnitText();
      int cpuNow = getDisplayTenths(latestSensors.cpuTenthsC), cpuPeak = getDisplayTenths(summary->peakCpuTenthsC);
      int rsxNow = getDisplayTenths(latestSensors.rsxTenthsC), rsxPeak = getDisplayTenths(summary->peakRsxTenthsC);

      snprintf(text, sizeof text, "%d.%d / %d.%d %s", cpuNow / 10, cpuNow % 10, cpuPeak / 10, cpuPeak % 10, unit);
      setLabelText(&statValues[STAT_CPU], text);
      snprintf(text, sizeof text, "%d.%d / %d.%d %s", rsxNow / 10, rsxNow % 10, rsxPeak / 10, rsxPeak % 10, unit);
      setLabelText(&statValues[STAT_RSX], text);
   }
   else
   {
      setLabelText(&statValues[STAT_CPU], "unreadable");
      setLabelText(&statValues[STAT_RSX], "unreadable");
   }

   // fan and clocks
   if (latestSensors.fanReadable)
   {
      const char *fanMode = getFanModeText(latestSensors.fanMode);
      snprintf(text, sizeof text, "%d / %d %%  (%s)", latestSensors.fanPercent, summary->peakFanPercent, fanMode);
      setLabelText(&statValues[STAT_FAN], text);
   }
   else setLabelText(&statValues[STAT_FAN], "unreadable");

   setClockValue(STAT_CORE_CLOCK, latestSensors.coreClockMhz, getBootRsxCoreClockMhz());
   setClockValue(STAT_MEMORY_CLOCK, latestSensors.memoryClockMhz, getBootRsxMemoryClockMhz());

   // the two right-hand lines: what is loaded, and how long a frame is taking
   const LoadState *load = getLoadState();
   snprintf(text, sizeof text, "CELL: %s - SPU: %d/%d - RSX: %s",
            getLoadLevelName(load->cpuLevel), load->spuThreadCount, MAX_SPU_THREADS, getLoadLevelName(load->gpuLevel));
   setLabelText(&loadLabel, text);
   int rightEdge = getGfxScreenWidth() - layout.sideMargin;
   moveLabel(&loadLabel, rightEdge - loadLabel.tt.tex.w, layout.titleTop);

   // frame time is the only visible sign that the gpu load is actually landing
   snprintf(text, sizeof text, "%d ms/frame", frameMs);
   setLabelText(&frameLabel, text);
   moveLabel(&frameLabel, rightEdge - frameLabel.tt.tex.w, layout.subtitleTop);

   refreshNotice();
}

// re-ranges the axis and rebuilds the tick captions. this rasterises text, so it
// belongs here in the update path and never in the draw path.
static void refreshGraph(void)
{
   updateGraph(&graph, getRunSamples(), getBaselineSamples());
}

static void setTimeWindow(int choice)
{
   windowChoice = (choice + WINDOW_CHOICE_COUNT) % WINDOW_CHOICE_COUNT;
   graph.windowSeconds = WINDOW_CHOICES[windowChoice];

   char caption[32];
   snprintf(caption, sizeof caption, "Time (%dm)", graph.windowSeconds / 60);
   setButtonHintCaption(&settingsHints, timeWindowHintIndex, caption);
   refreshGraph();
}

// the key is a row of swatch + name pairs, centred above the graph. the layout
// is walked once here and the swatch positions kept, so drawing does no maths.
static void layoutKey(int screenWidth, int keyY)
{
   int keyWidth = KEY_ENTRY_GAP * (KEY_ENTRY_COUNT - 1);
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++)
      keyWidth += KEY_SWATCH_WIDTH + KEY_SWATCH_GAP + keyLabels[entry].tt.tex.w;

   int keyX = (screenWidth - keyWidth) / 2;
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++)
   {
      keySwatchX[entry] = keyX;
      moveLabel(&keyLabels[entry], keyX + KEY_SWATCH_WIDTH + KEY_SWATCH_GAP, keyY);
      keyX += KEY_SWATCH_WIDTH + KEY_SWATCH_GAP + keyLabels[entry].tt.tex.w + KEY_ENTRY_GAP;
   }
}

// every position on the screen comes from the screen's own size, so the layout
// holds up at any resolution.
static void computeBenchLayout(int screenWidth, int screenHeight)
{
   layout.sideMargin     = screenWidth / 40;    // the screen edges are tighter than the top and bottom
   layout.verticalMargin = screenWidth / 24;
   layout.textSize       = screenHeight / 40;
   layout.subtitleSize   = layout.textSize * 4 / 5;
   layout.spacing        = layout.textSize * 5 / 3;
   layout.headerTop      = layout.verticalMargin - HEADER_LIFT_PIXELS;
   layout.titleTop       = layout.headerTop - layout.spacing / 2;
   layout.subtitleTop    = layout.titleTop + layout.spacing * 4 / 5 + SUBTITLE_GAP_PIXELS;
}

static void initBenchLabels(int valueColumnX)
{
   int left = layout.sideMargin, size = layout.textSize, small = layout.subtitleSize;

   initLabel(&title, &font, left, layout.titleTop, AUTO, AUTO, size + 8, COLOR_WHITE, TEXT_NOWRAP, "Thermal Bench");

   // the title is bigger than the load line opposite it, so its subtitle sits lower
   int modelTop = layout.subtitleTop + SUBTITLE_GAP_PIXELS;
   initLabelRaw(&modelLabel, &font, left, modelTop, AUTO, AUTO, small, SUBTITLE_COLOR, TEXT_NOWRAP, "");

   int statTop = layout.headerTop + layout.spacing * 2;
   for (int row = 0; row < STAT_ROW_COUNT; row++)
   {
      int rowY = statTop + layout.spacing * row;
      uint32_t color = STAT_COLORS[row];
      initLabel(&statNames[row], &font, left, rowY, AUTO, AUTO, size, color, TEXT_NOWRAP, STAT_NAMES[row]);
      initLabelRaw(&statValues[row], &font, valueColumnX, rowY, AUTO, AUTO, size, color, TEXT_NOWRAP, "");
   }

   initLabelRaw(&loadLabel, &font, left, layout.titleTop, AUTO, AUTO, size, COLOR_AMBER_300, TEXT_NOWRAP, "");
   initLabelRaw(&frameLabel, &font, left, layout.subtitleTop, AUTO, AUTO, small, SUBTITLE_COLOR, TEXT_NOWRAP, "");
   initLabelRaw(&noticeLabel, &font, left, statTop, AUTO, AUTO, size, COLOR_AMBER_300, TEXT_NOWRAP, "");
}

// two rows, each centred on its own: what the run is set up like on top, the
// three load toggles underneath. the clock hints lead the first row; each is a
// caption-less Select clustered with its d-pad glyph, which is what a
// caption-less hint means to the widget.
static void initBenchHints(int screenHeight)
{
   int glyphHeight = layout.textSize + 4;
   int bottomRowY = screenHeight - layout.verticalMargin;
   layout.hintsTop = bottomRowY - glyphHeight - HINT_ROW_GAP_PIXELS;

   initButtonHints(&settingsHints, &font, layout.hintsTop, glyphHeight, layout.textSize, COLOR_SLATE_300);
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_SELECT), "");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_DPAD_UP), "Overclock RSX");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_SELECT), "");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_DPAD_RIGHT), "Overclock Mem");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_START), "Toggle Units");
   addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_L1), "");
   timeWindowHintIndex = addButtonHint(&settingsHints, getConsoleGlyph(GLYPH_R1), "Time");

   initButtonHints(&loadHints, &font, bottomRowY, glyphHeight, layout.textSize, COLOR_SLATE_300);
   addButtonHint(&loadHints, getConsoleGlyph(GLYPH_CROSS), "Parallel Load");
   addButtonHint(&loadHints, getConsoleGlyph(GLYPH_SQUARE), "CELL Load");
   addButtonHint(&loadHints, getConsoleGlyph(GLYPH_TRIANGLE), "RSX Load");
}

static void initBench(void)
{
   font = openSystemFont(FONT_POP);

   // layout, then the widgets that hang off it
   int screenWidth = getGfxScreenWidth(), screenHeight = getGfxScreenHeight();
   computeBenchLayout(screenWidth, screenHeight);
   initBenchLabels(layout.sideMargin + layout.textSize * 15 / 2);   // room for the longest name ("RSX Clock")

   initBenchHints(screenHeight);

   // graph, with the key centred above it: it fills what is left between the
   // last stat row and the top hint row
   int graphX = layout.sideMargin + GRAPH_AXIS_GUTTER;
   int keyRowHeight = layout.textSize * 2;
   int graphY = layout.headerTop + layout.spacing * (2 + STAT_ROW_COUNT) + keyRowHeight;
   int graphHeight = layout.hintsTop - layout.spacing - graphY;
   int graphWidth = screenWidth - graphX - layout.sideMargin - GRAPH_AXIS_GUTTER;
   initGraph(&graph, &font, layout.textSize, graphX, graphY, graphWidth, graphHeight, WINDOW_CHOICES[windowChoice]);

   int keyY = graphY - keyRowHeight + layout.spacing / 3;   // inside the row reserved for it, just above the plot
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++)
   {
      StatRow row = KEY_ROWS[entry];
      int size = layout.textSize;
      initLabel(&keyLabels[entry], &font, 0, keyY, AUTO, AUTO, size, STAT_COLORS[row], TEXT_NOWRAP, STAT_NAMES[row]);
   }
   layoutKey(screenWidth, keyY);

   // start the run
   lastReadoutUs = sys_time_get_system_time();
   framesSinceReadout = 0;

   initStress();
   initClocks();
   readSensors(&latestSensors);
   startMetricsLog(&latestSensors, getLoadState());

   setTimeWindow(windowChoice);
   refreshModelLabel();
   refreshReadouts();
}

// select + d-pad tunes the graphics clocks; without select the d-pad is free
static void handleBenchInput(void)
{
   if (isPadButtonDown(PAD_BTN_SELECT))
   {
      if (isPadButtonPressed(PAD_BTN_UP))    stepRsxCoreClock(+1);
      if (isPadButtonPressed(PAD_BTN_DOWN))  stepRsxCoreClock(-1);
      if (isPadButtonPressed(PAD_BTN_RIGHT)) stepRsxMemoryClock(+1);
      if (isPadButtonPressed(PAD_BTN_LEFT))  stepRsxMemoryClock(-1);
      return;
   }

   if (isPadButtonPressed(PAD_BTN_START))
   {
      toggleTemperatureUnit();
      refreshModelLabel();
      refreshReadouts();
      refreshGraph();
   }
   if (isPadButtonPressed(PAD_BTN_R1)) setTimeWindow(windowChoice + 1);
   if (isPadButtonPressed(PAD_BTN_L1)) setTimeWindow(windowChoice - 1);

   if (isPadButtonPressed(PAD_BTN_CROSS))    stepBothLoads();
   if (isPadButtonPressed(PAD_BTN_SQUARE))   stepCpuLoad();
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) stepGpuLoad();
}

static void updateBench(void)
{
   // while the XMB is open over us the console belongs to it: hand back the cpu
   // and the frame rate, or it cannot draw and the screen looks hung.
   if (appSystemMenuOpen != menuWasOpen)
   {
      menuWasOpen = appSystemMenuOpen;
      setStressSuspended(menuWasOpen);
      if (!menuWasOpen)
      {
         // the menu was open for minutes and drew none of our frames; without
         // this the next frame time is the whole menu period divided by three.
         lastReadoutUs = sys_time_get_system_time();
         framesSinceReadout = 0;
      }
   }
   if (menuWasOpen) { sleepMs(16); return; }

   framesSinceReadout++;

   // readouts refresh a few times a second, not every frame: each refresh
   // re-rasterises text, and uncapped gpu load can push the frame rate high.
   uint64_t now = sys_time_get_system_time();
   if (now - lastReadoutUs >= READOUT_INTERVAL_US)
   {
      if (framesSinceReadout > 0) frameMs = (int)((now - lastReadoutUs) / 1000) / framesSinceReadout;
      framesSinceReadout = 0;
      lastReadoutUs = now;

      readSensors(&latestSensors);
      logSensorChanges(&latestSensors);
      updateSafety(&latestSensors);
      recordMetricsSample(&latestSensors, frameMs, getLoadState());

      refreshReadouts();
      refreshGraph();
   }

   handleBenchInput();
}

static void drawKey(void)
{
   int swatchY = keyLabels[0].y + layout.textSize / 2;
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++)
   {
      fillGfxRectangle(keySwatchX[entry], swatchY, KEY_SWATCH_WIDTH, 3, STAT_COLORS[KEY_ROWS[entry]]);
      drawLabel(&keyLabels[entry]);
   }
}

static void drawBench(void)
{
   drawStressCanvas();
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), SCRIM_COLOR);

   drawLabel(&title);
   drawLabel(&modelLabel);
   drawLabel(&loadLabel);
   drawLabel(&frameLabel);
   drawLabel(&noticeLabel);
   for (int row = 0; row < STAT_ROW_COUNT; row++) { drawLabel(&statNames[row]); drawLabel(&statValues[row]); }

   drawKey();
   drawGraph(&graph, getRunSamples(), getBaselineSamples());
   drawButtonHints(&settingsHints, getGfxScreenWidth());
   drawButtonHints(&loadHints, getGfxScreenWidth());
}

// the run is saved here rather than on a quit button: leaving via the PS button
// tears the screen down, so this is the one path every exit goes through.
static void termBench(void)
{
   stopStress();
   finishMetricsLog();

   termButtonHints(&settingsHints);
   termButtonHints(&loadHints);
   freeGraph(&graph);
   freeLabel(&title);
   freeLabel(&modelLabel);
   freeLabel(&loadLabel);
   freeLabel(&frameLabel);
   freeLabel(&noticeLabel);
   for (int row = 0; row < STAT_ROW_COUNT; row++) { freeLabel(&statNames[row]); freeLabel(&statValues[row]); }
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++) freeLabel(&keyLabels[entry]);
   closeFont(&font);
}

Screen benchScreen = { initBench, NULL, updateBench, drawBench, NULL, termBench, SCREEN_TERMINATED };
