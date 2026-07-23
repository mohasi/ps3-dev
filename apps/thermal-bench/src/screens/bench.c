// bench screen - the main view. a full-screen stress canvas is drawn behind a
// semi-transparent scrim; the live readouts, a key and the graph float on top,
// like a furmark overlay. reads the sensors every 500 ms, records a graph point
// and a csv row every 2 seconds, and saves the run summary on exit.
#include <sys/sys_time.h>
#include <stdio.h>
#include <string.h>

#include "screens/bench.h"
#include "sensors.h"
#include "console-model.h"
#include "settings.h"
#include "graph.h"
#include "metrics-log.h"
#include "stress.h"
#include "clocks.h"
#include "app.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "thread.h"
#include "dbg.h"

#define SAMPLE_INTERVAL_SECONDS 2
#define READOUT_INTERVAL_US 500000
#define MAX_SAMPLES 4096
#define SCRIM_COLOR 0x80000000     // 50% black over the stress canvas

// the two lines under the title and the load are subtitles: smaller, and a touch
// faded so they sit behind the readings they explain.
#define SUBTITLE_COLOR ((COLOR_SLATE_300 & 0x00FFFFFF) | 0xE5000000)   // 90% opacity

// the readouts, in display order. the three that have a graph line take that
// line's colour so a number and its trace read as the same thing.
typedef enum StatRow { STAT_CPU, STAT_RSX, STAT_FAN, STAT_CORE_CLOCK, STAT_MEMORY_CLOCK, STAT_ROW_COUNT } StatRow;

static const char *STAT_NAMES[STAT_ROW_COUNT] = { "CELL", "RSX", "Fan", "RSX Clock", "Mem Clock" };
static const uint32_t STAT_COLORS[STAT_ROW_COUNT] = { GRAPH_COLOR_CPU, GRAPH_COLOR_RSX, GRAPH_COLOR_FAN, COLOR_SLATE_300, COLOR_SLATE_300 };

// the three readings that have a graph line, for the key above the graph
#define KEY_ENTRY_COUNT 3
static const StatRow KEY_ROWS[KEY_ENTRY_COUNT] = { STAT_CPU, STAT_RSX, STAT_FAN };

static const int WINDOW_CHOICES[] = { 120, 300, 600, 1800 };
#define WINDOW_CHOICE_COUNT (int)(sizeof WINDOW_CHOICES / sizeof WINDOW_CHOICES[0])

#define HINT_TIME_INDEX    6   // the R1 hint, whose caption carries the current window
#define KEY_SWATCH_WIDTH   26
#define KEY_SWATCH_GAP      8
#define KEY_ENTRY_GAP      34

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
static ButtonHints hints;

static GraphSample samples[MAX_SAMPLES];
static int sampleCount;
static GraphSample baseline[MAX_SAMPLES];
static int baselineCount;

static uint64_t startUs;
static uint64_t lastReadoutUs;
static int framesSinceReadout;
static int frameMs;
static int lastSampleSecond;
static int windowChoice = 1;
static int menuWasOpen;
static int cutoffFired;

static Sensors latest;
static RunSummary summary;
static int startFanPercent;

static int margin, sideMargin, textSize, subtitleSize, spacing, headerTop, titleTop, subtitleTop;

static int elapsedSeconds(void)
{
   return (int)((sys_time_get_system_time() - startUs) / 1000000);
}

static void recordSample(void)
{
   int seconds = elapsedSeconds();

   GraphSample point = { (uint32_t)seconds, (uint16_t)latest.cpuTenthsC, (uint16_t)latest.rsxTenthsC, (uint8_t)latest.fanPercent };
   if (sampleCount == MAX_SAMPLES) { memmove(samples, samples + 1, sizeof(GraphSample) * (MAX_SAMPLES - 1)); sampleCount--; }
   samples[sampleCount++] = point;

   appendMetricsSample(seconds, &latest, frameMs, getLoadState());

   if (latest.cpuTenthsC > summary.peakCpuTenthsC) summary.peakCpuTenthsC = latest.cpuTenthsC;
   if (latest.rsxTenthsC > summary.peakRsxTenthsC) summary.peakRsxTenthsC = latest.rsxTenthsC;
   if (latest.fanPercent > summary.peakFanPercent) summary.peakFanPercent = latest.fanPercent;
   if (summary.firstFanStepSeconds < 0 && latest.fanReadable && latest.fanPercent > startFanPercent) summary.firstFanStepSeconds = seconds;
   summary.durationSeconds = seconds;
}

// a stress test must never be the thing that cooks the console: past this
// temperature - or if the console will not tell us the temperature at all - the
// load drops itself to off and the clocks go back to what they booted at.
static void cutLoadIfTooHot(void)
{
   const LoadState *load = getLoadState();
   if (load->cpuLevel == 0 && load->gpuLevel == 0) return;

   int cutoffTenths = getSafetyCutoffCelsius() * 10;
   int tooHot = latest.cpuTenthsC >= cutoffTenths || latest.rsxTenthsC >= cutoffTenths;
   if (latest.temperatureReadable && !tooHot) return;

   if (latest.temperatureReadable)
      logWarn("[bench] safety cutoff: cpu %d.%d C, rsx %d.%d C - dropping load to off\n",
              latest.cpuTenthsC / 10, latest.cpuTenthsC % 10, latest.rsxTenthsC / 10, latest.rsxTenthsC % 10);
   else
      logWarn("[bench] safety cutoff: the console refused to report its temperature - dropping load to off\n");

   setCpuLoad(0);
   setGpuLoad(0);
   restoreBootRsxClocks();
   cutoffFired = 1;
}

// one line for whatever the user cannot otherwise see: why the load switched
// itself off, that the run is not being recorded, that the fan is not going to
// respond, that a clock change outlives the app.
static void refreshNotice(void)
{
   const LoadState *load = getLoadState();
   char cutoffText[LABEL_MAX_TEXT];
   const char *text = "";

   if (cutoffFired && !latest.temperatureReadable)
      text = "safety cutoff: temperature unreadable - load off, clocks back to boot";
   else if (cutoffFired)
   {
      snprintf(cutoffText, sizeof cutoffText, "safety cutoff at %d %s - load off, clocks back to boot",
               getDisplayTenths(getSafetyCutoffCelsius() * 10) / 10, getTemperatureUnitText());
      text = cutoffText;
   }
   else if (!isMetricsLogRecording())
      text = "not recording - this run is not being saved";
   else if (latest.coreClockMhz != getBootRsxCoreClockMhz() || latest.memoryClockMhz != getBootRsxMemoryClockMhz())
      text = "clocks changed - the new clocks stay after exit, until the console reboots";
   else if (load->cpuLevel + load->gpuLevel > 0 && latest.fanReadable && latest.fanMode == FAN_MODE_MANUAL)
      text = "the fan is set manually - it will not step up as the console heats";

   setLabelText(&noticeLabel, text);
   moveLabel(&noticeLabel, getGfxScreenWidth() - sideMargin - noticeLabel.tt.tex.w, headerTop + spacing * 2);
}

// which console this is and the temperature its load switches off at. static text,
// so it is only rebuilt when the displayed unit changes.
static void refreshModelLabel(void)
{
   char text[LABEL_MAX_TEXT];
   snprintf(text, sizeof text, "%s - cutoff %d %s%s", getConsoleModelSummary(),
            getDisplayTenths(getSafetyCutoffCelsius() * 10) / 10, getTemperatureUnitText(),
            isSafetyCutoffFromSettings() ? " (overridden in settings.txt)" : "");
   setLabelText(&modelLabel, text);
}

static void refreshReadouts(void)
{
   char text[LABEL_MAX_TEXT];

   if (latest.temperatureReadable)
   {
      const char *unit = getTemperatureUnitText();
      int cpuNow = getDisplayTenths(latest.cpuTenthsC), cpuPeak = getDisplayTenths(summary.peakCpuTenthsC);
      int rsxNow = getDisplayTenths(latest.rsxTenthsC), rsxPeak = getDisplayTenths(summary.peakRsxTenthsC);

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

   if (latest.fanReadable)
   {
      snprintf(text, sizeof text, "%d / %d %%  (%s)", latest.fanPercent, summary.peakFanPercent, getFanModeText(latest.fanMode));
      setLabelText(&statValues[STAT_FAN], text);
   }
   else setLabelText(&statValues[STAT_FAN], "unreadable");

   // the boot value sits alongside, because a clock change here is global and
   // stays after the app closes
   snprintf(text, sizeof text, "%d MHz  (boot %d)", latest.coreClockMhz, getBootRsxCoreClockMhz());
   setLabelText(&statValues[STAT_CORE_CLOCK], text);
   snprintf(text, sizeof text, "%d MHz  (boot %d)", latest.memoryClockMhz, getBootRsxMemoryClockMhz());
   setLabelText(&statValues[STAT_MEMORY_CLOCK], text);

   const LoadState *load = getLoadState();
   snprintf(text, sizeof text, "CELL: %s - SPU: %d/%d - RSX: %s", getLoadLevelName(load->cpuLevel), load->spuLevel, MAX_SPU_THREADS, getLoadLevelName(load->gpuLevel));
   setLabelText(&loadLabel, text);
   moveLabel(&loadLabel, getGfxScreenWidth() - sideMargin - loadLabel.tt.tex.w, titleTop);

   // frame time sits under the load: it is the only visible sign that the gpu
   // load is actually landing
   snprintf(text, sizeof text, "%d ms/frame", frameMs);
   setLabelText(&frameLabel, text);
   moveLabel(&frameLabel, getGfxScreenWidth() - sideMargin - frameLabel.tt.tex.w, subtitleTop);

   refreshNotice();
}

// re-ranges the axis and rebuilds the tick captions. this rasterises text, so it
// belongs here in the update path and never in the draw path.
static void refreshGraph(void)
{
   updateGraph(&graph, samples, sampleCount, baseline, baselineCount);
}

static void setTimeWindow(int choice)
{
   windowChoice = (choice + WINDOW_CHOICE_COUNT) % WINDOW_CHOICE_COUNT;
   graph.windowSeconds = WINDOW_CHOICES[windowChoice];

   char caption[32];
   snprintf(caption, sizeof caption, "Time (%dm)", graph.windowSeconds / 60);
   setButtonHintCaption(&hints, HINT_TIME_INDEX, caption);
   refreshGraph();
}

// the key is a row of swatch + name pairs, centred above the graph. the layout
// is walked once here and the swatch positions kept, so drawing does no maths.
static void layoutKey(int screenWidth, int keyY)
{
   int keyWidth = KEY_ENTRY_GAP * (KEY_ENTRY_COUNT - 1);
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++) keyWidth += KEY_SWATCH_WIDTH + KEY_SWATCH_GAP + keyLabels[entry].tt.tex.w;

   int keyX = (screenWidth - keyWidth) / 2;
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++)
   {
      keySwatchX[entry] = keyX;
      moveLabel(&keyLabels[entry], keyX + KEY_SWATCH_WIDTH + KEY_SWATCH_GAP, keyY);
      keyX += KEY_SWATCH_WIDTH + KEY_SWATCH_GAP + keyLabels[entry].tt.tex.w + KEY_ENTRY_GAP;
   }
}

static void initBench(void)
{
   font = openSystemFont(FONT_POP);

   int screenW = getGfxScreenWidth(), screenH = getGfxScreenHeight();
   margin       = screenW / 24;
   sideMargin   = screenW / 40;                 // the screen edges are tighter than the top and bottom
   textSize     = screenH / 40;
   subtitleSize = textSize * 4 / 5;
   spacing      = textSize * 5 / 3;
   headerTop    = margin - 30;                  // the stats and the notice line sit on this line
   titleTop     = headerTop - spacing / 2;      // the title and the load sit higher, with their subtitle under them
   subtitleTop  = titleTop + spacing * 4 / 5 + 5;
   int valueColumnX = sideMargin + textSize * 15 / 2;   // room for the longest name ("RSX Clock")

   initLabel(&title, &font, sideMargin, titleTop, AUTO, AUTO, textSize + 8, COLOR_WHITE, TEXT_NOWRAP, "Thermal Bench");
   // the title is bigger than the load line opposite it, so its subtitle needs a touch more gap
   initLabelRaw(&modelLabel, &font, sideMargin, subtitleTop + 5, AUTO, AUTO, subtitleSize, SUBTITLE_COLOR, TEXT_NOWRAP, "");

   int statTop = headerTop + spacing * 2;
   for (int row = 0; row < STAT_ROW_COUNT; row++)
   {
      initLabel(&statNames[row], &font, sideMargin, statTop + spacing * row, AUTO, AUTO, textSize, STAT_COLORS[row], TEXT_NOWRAP, STAT_NAMES[row]);
      initLabelRaw(&statValues[row], &font, valueColumnX, statTop + spacing * row, AUTO, AUTO, textSize, STAT_COLORS[row], TEXT_NOWRAP, "");
   }
   initLabelRaw(&loadLabel, &font, sideMargin, titleTop, AUTO, AUTO, textSize, COLOR_AMBER_300, TEXT_NOWRAP, "");
   initLabelRaw(&frameLabel, &font, sideMargin, subtitleTop, AUTO, AUTO, subtitleSize, SUBTITLE_COLOR, TEXT_NOWRAP, "");
   initLabelRaw(&noticeLabel, &font, sideMargin, headerTop + spacing * 2, AUTO, AUTO, textSize, COLOR_AMBER_300, TEXT_NOWRAP, "");

   // graph, with the key centred above it
   int graphX = sideMargin + 50;
   int graphH = (screenH - margin * 2) * 55 / 100;
   int graphY = screenH - margin - spacing - graphH;
   int graphW = screenW - graphX - sideMargin - 50;
   initGraph(&graph, &font, textSize, graphX, graphY, graphW, graphH, WINDOW_CHOICES[windowChoice]);

   int keyY = graphY - textSize * 2 - spacing / 3;
   for (int entry = 0; entry < KEY_ENTRY_COUNT; entry++)
      initLabel(&keyLabels[entry], &font, 0, keyY, AUTO, AUTO, textSize, STAT_COLORS[KEY_ROWS[entry]], TEXT_NOWRAP, STAT_NAMES[KEY_ROWS[entry]]);
   layoutKey(screenW, keyY);

   // the clock hints lead the row; each is a caption-less Select clustered with its d-pad glyph
   initButtonHints(&hints, &font, screenH - margin, textSize + 4, textSize, COLOR_SLATE_300);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT), "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_DPAD_UP), "Overclock RSX");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT), "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_DPAD_RIGHT), "Overclock Mem");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START), "Toggle Units");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_L1), "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R1), "Time");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS), "Parallel Load");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), "CELL Load");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "RSX Load");

   sampleCount = 0;
   lastSampleSecond = -SAMPLE_INTERVAL_SECONDS;
   summary.firstFanStepSeconds = -1;
   startUs = sys_time_get_system_time();
   lastReadoutUs = startUs;
   framesSinceReadout = 0;

   initClocks();
   readSensors(&latest);
   startFanPercent = latest.fanPercent;
   startMetricsLog(&latest, getLoadState());
   baselineCount = loadPreviousRun(baseline, MAX_SAMPLES);

   setTimeWindow(windowChoice);
   refreshModelLabel();
   refreshReadouts();
}

static void updateBench(void)
{
   // while the XMB is open over us the console belongs to it: hand back the cpu
   // and the frame rate, or it cannot draw and the screen looks hung.
   if (appSystemMenuOpen != menuWasOpen)
   {
      menuWasOpen = appSystemMenuOpen;
      if (menuWasOpen) suspendStress();
      else
      {
         resumeStress();
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

      readSensors(&latest);
      logSensorChanges(&latest);
      cutLoadIfTooHot();

      int seconds = elapsedSeconds();
      if (seconds - lastSampleSecond >= SAMPLE_INTERVAL_SECONDS)
      {
         lastSampleSecond = seconds;
         recordSample();
      }
      refreshReadouts();
      refreshGraph();
   }

   // select + d-pad tunes the graphics clocks; without select the d-pad is free
   if (isPadButtonDown(PAD_BTN_SELECT))
   {
      if (isPadButtonPressed(PAD_BTN_UP))    stepRsxCoreClock(+1);
      if (isPadButtonPressed(PAD_BTN_DOWN))  stepRsxCoreClock(-1);
      if (isPadButtonPressed(PAD_BTN_RIGHT)) stepRsxMemoryClock(+1);
      if (isPadButtonPressed(PAD_BTN_LEFT))  stepRsxMemoryClock(-1);
      return;
   }

   if (isPadButtonPressed(PAD_BTN_START)) { toggleTemperatureUnit(); refreshModelLabel(); refreshReadouts(); refreshGraph(); }
   if (isPadButtonPressed(PAD_BTN_R1)) setTimeWindow(windowChoice + 1);
   if (isPadButtonPressed(PAD_BTN_L1)) setTimeWindow(windowChoice - 1);
   const LoadState *load = getLoadState();
   if (isPadButtonPressed(PAD_BTN_CROSS))    { int next = load->cpuLevel >= load->gpuLevel ? load->cpuLevel + 1 : load->gpuLevel + 1; setCpuLoad(next); setGpuLoad(next); cutoffFired = 0; }
   if (isPadButtonPressed(PAD_BTN_SQUARE))   { setCpuLoad(load->cpuLevel + 1); cutoffFired = 0; }
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { setGpuLoad(load->gpuLevel + 1); cutoffFired = 0; }
}

static void drawKey(void)
{
   int swatchY = keyLabels[0].y + textSize / 2;
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
   drawGraph(&graph, samples, sampleCount, baselineCount > 0 ? baseline : NULL, baselineCount);
   drawButtonHints(&hints, getGfxScreenWidth());
}

// the run is saved here rather than on a quit button: leaving via the PS button
// tears the screen down, so this is the one path every exit goes through.
static void termBench(void)
{
   stopStress();
   summary.durationSeconds = elapsedSeconds();
   finishMetricsLog(&summary);

   termButtonHints(&hints);
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
