// home - the downloads, what has finished, the search results and the settings, one view at a time.

#include "screens/home.h"

#include <math.h>   // sinf and cosf, for the ring around a torrent's percentage

#include "colors.h"
#include "font.h"
#include "format.h"
#include "gfx.h"
#include "log-store.h"
#include "osk-input.h"
#include "pad.h"
#include "settings.h"
#include "string-utilities.h"
#include "torrent-service.h"
#include "button-repeat.h"
#include "ui/icon-font.h"
#include "ui/label.h"
#include "widgets/ask-overlay.h"
#include "widgets/chrome.h"
#include "widgets/theme.h"

#define ROWS_VISIBLE   7
#define ROW_HEIGHT   116
#define ROW_TOP      (CONTENT_TOP + 14)
#define TILE_X       (CONTENT_X + 14)
#define TILE_WIDTH   (CONTENT_WIDTH - 28)

#define RING_CENTRE_X (TILE_X + 52)
#define RING_RADIUS    34
#define RING_THICKNESS  5
#define RING_TEXT_SIZE 18

#define TEXT_X      (TILE_X + 110)
#define TEXT_RIGHT  (TILE_X + TILE_WIDTH - 20)
#define TEXT_WIDTH  (TEXT_RIGHT - TEXT_X)
#define BAR_HEIGHT   8

#define TITLE_SIZE 24
#define LINE_SIZE  20
#define ARROW_SIZE 18

#define SETTINGS_LINES  5
#define SETTINGS_KEY_X  (CONTENT_X + 26)
#define SETTINGS_VALUE_X (SETTINGS_KEY_X + 230)
#define SETTINGS_STEP   44

#define LOG_LINES_VISIBLE 26
#define LOG_LINE_HEIGHT   30
#define LOG_TEXT_SIZE     18

static Font font;
static Label emptyLabel;
static Label nameLabels[ROWS_VISIBLE];
static Label statLabels[ROWS_VISIBLE];
static Label sizeLabels[ROWS_VISIBLE];
static Label ringLabels[ROWS_VISIBLE];
static Label downSpeedLabels[ROWS_VISIBLE];
static Label upSpeedLabels[ROWS_VISIBLE];
static Icon downIcon;
static Icon upIcon;
static Label settingKeyLabels[SETTINGS_LINES];
static Label settingValueLabels[SETTINGS_LINES];

static const char *SETTING_NAMES[SETTINGS_LINES] = { "Download Dir:", "Search Sources Dir:", "Wireguard Config:",
                                                     "VPN Mode:", "Secure Delete:" };
static Label logLabels[LOG_LINES_VISIBLE];

static char query[SOURCE_QUERY_MAX];
static int searchRan;                 // so an empty result reads as "nothing found" rather than "search first"
static int selected[VIEW_COUNT];
static int scrollTop[VIEW_COUNT];
static int addedResult = -1;
static int downloadsBeforeAdd = -1;   // how many there were when a result was added, until it shows up
static int slotToDelete = -1;         // what the delete question is about, while it is up
static ButtonRepeat downRepeat, upRepeat;   // holding the pad walks the list rather than one row a press

// section: which torrents a view shows

static int isCompleted(const ServiceTorrent *torrent)
{
   return torrent->status == TORRENT_FINISHED;
}

// the torrents in this view, as slot numbers, so a row knows which torrent it is showing
static int listView(AppView view, int *slots, int capacity)
{
   int count = 0;

   for (int slot = 0; slot < getServiceTorrentCount() && count < capacity; slot++) {
      ServiceTorrent torrent;
      getServiceTorrent(slot, &torrent);

      int wanted = view == VIEW_COMPLETED ? isCompleted(&torrent) : !isCompleted(&torrent);
      if (wanted) slots[count++] = slot;
   }

   return count;
}

static int getRowCount(AppView view)
{
   if (view == VIEW_SEARCH) return getServiceResultCount();
   if (view == VIEW_SETTINGS || view == VIEW_LOGS) return 0;

   int slots[TORRENT_SLOT_MAX];
   return listView(view, slots, TORRENT_SLOT_MAX);
}

// section: the parts of a row

static const char *getStatusName(TorrentStatus status)
{
   static const char *NAMES[] = { "waiting", "asking a tracker", "asking what it is", "downloading", "paused",
                                  "finished", "failed" };
   return NAMES[status];
}

static int getDonePercent(const ServiceTorrent *torrent)
{
   if (torrent->pieceCount <= 0) return 0;
   return torrent->piecesDone * 100 / torrent->pieceCount;
}

static uint32_t getProgressColor(const ServiceTorrent *torrent)
{
   if (torrent->status == TORRENT_FAILED) return COLOR_RED_500;
   if (torrent->status == TORRENT_FINISHED) return COLOR_EMERALD_500;
   if (torrent->status == TORRENT_PAUSED || torrent->checking) return TEXT_FAINT;

   return COLOR_SKY_500;
}

// the row the pad is on: a blue box while the list has the pad, otherwise the same line every row has
static void drawRowFrame(int y, int isSelected)
{
   if (isSelected && getChromeFocus() == FOCUS_LIST) {
      drawGfxBox(TILE_X, y, TILE_WIDTH, ROW_HEIGHT, 2, PICKED_FILL, PICKED_BORDER);
      return;
   }

   uint32_t line = isSelected ? RESTING_BORDER : ROW_SEPARATOR;
   drawGfxLine(TILE_X, y + ROW_HEIGHT, TILE_X + TILE_WIDTH, y + ROW_HEIGHT, 1, line);
}

// the ring around the percentage: a band of wedges from the top clockwise, filled as far as percent
static void drawProgressRing(int centreY, int percent, uint32_t color)
{
   static const int STEPS = 64;
   static const float TWO_PI = 6.2831853f;

   float inner = RING_RADIUS - RING_THICKNESS;

   for (int step = 0; step < STEPS; step++) {
      float from = TWO_PI * step / STEPS - TWO_PI / 4;
      float to = TWO_PI * (step + 1) / STEPS - TWO_PI / 4;
      uint32_t wedge = step * 100 / STEPS < percent ? color : COLOR_NEUTRAL_800;

      float innerFromX = RING_CENTRE_X + inner * cosf(from), innerFromY = centreY + inner * sinf(from);
      float outerFromX = RING_CENTRE_X + RING_RADIUS * cosf(from), outerFromY = centreY + RING_RADIUS * sinf(from);
      float innerToX = RING_CENTRE_X + inner * cosf(to), innerToY = centreY + inner * sinf(to);
      float outerToX = RING_CENTRE_X + RING_RADIUS * cosf(to), outerToY = centreY + RING_RADIUS * sinf(to);

      drawGfxTriangle(innerFromX, innerFromY, wedge, outerFromX, outerFromY, wedge, outerToX, outerToY, wedge);
      drawGfxTriangle(innerFromX, innerFromY, wedge, outerToX, outerToY, wedge, innerToX, innerToY, wedge);
   }
}

// what a row says a torrent is doing, and why when that is a failure
static void appendStatus(char *out, int capacity, int *offset, const ServiceTorrent *torrent)
{
   appendStr(out, capacity, offset, getStatusName(torrent->status));

   // a failure that says only "failed" tells nobody anything
   if (torrent->status != TORRENT_FAILED || !torrent->reason[0]) return;

   appendStr(out, capacity, offset, ": ");
   appendStr(out, capacity, offset, torrent->reason);
}

// "ETA: 4m 12s   Peers: 24 (2)", the part of the third row that follows the two speeds
static void writeStatLine(char *out, int capacity, const ServiceTorrent *torrent)
{
   int offset = 0;

   appendStr(out, capacity, &offset, "ETA: ");

   int64_t left = torrent->totalLength - torrent->bytesDone;
   if (torrent->bytesPerSecond > 0 && left > 0) {
      int seconds = (int)(left / torrent->bytesPerSecond);

      // hours once minutes would run into the hundreds
      if (seconds >= 3600) {
         offset = appendUint64(out, capacity, offset, (uint64_t)(seconds / 3600));
         appendStr(out, capacity, &offset, "h ");
         offset = appendUint64(out, capacity, offset, (uint64_t)(seconds % 3600) / 60);
         appendStr(out, capacity, &offset, "m");
      } else {
         offset = appendUint64(out, capacity, offset, (uint64_t)(seconds / 60));
         appendStr(out, capacity, &offset, "m ");
         offset = appendUint64(out, capacity, offset, (uint64_t)(seconds % 60));
         appendStr(out, capacity, &offset, "s");
      }
   } else {
      appendStr(out, capacity, &offset, "--");
   }

   appendStr(out, capacity, &offset, "   Peers: ");
   offset = appendUint64(out, capacity, offset, (uint64_t)torrent->peerCount);
   appendStr(out, capacity, &offset, " (");
   offset = appendUint64(out, capacity, offset, (uint64_t)torrent->connectedCount);
   appendStr(out, capacity, &offset, ")   Seeds: ");
   offset = appendUint64(out, capacity, offset, (uint64_t)torrent->seederCount);

   // what it is doing, unless the numbers above already say it
   if (torrent->checking) appendStr(out, capacity, &offset, "   checking what is already here");
   else if (torrent->status != TORRENT_DOWNLOADING) {
      appendStr(out, capacity, &offset, "   ");
      appendStatus(out, capacity, &offset, torrent);
   }

   out[offset] = 0;
}

// "9.4 MB/s v  0.3 MB/s ^", the two speeds with an arrow after each, then the rest of the line
static void drawStatRow(int row, int y)
{
   int x = TEXT_X;

   moveLabel(&downSpeedLabels[row], x, y);
   drawLabel(&downSpeedLabels[row]);

   x += downSpeedLabels[row].tt.tex.w + 4;
   drawIcon(&downIcon, x, y - 3, TEXT_QUIET);

   x += ARROW_SIZE + 22;
   moveLabel(&upSpeedLabels[row], x, y);
   drawLabel(&upSpeedLabels[row]);

   x += upSpeedLabels[row].tt.tex.w + 4;
   drawIcon(&upIcon, x, y - 3, TEXT_QUIET);

   moveLabel(&statLabels[row], x + ARROW_SIZE + 26, y);
   drawLabel(&statLabels[row]);
}

static int getRowY(int row)
{
   return ROW_TOP + row * ROW_HEIGHT;
}

// Every label a torrent row shows, filled before the frame starts. Text turns into a picture in
// video memory as it is set, and doing that while the frame is being drawn is what leaves torn
// streaks of old text on the screen.
static void fillTorrentRow(int row, int slot)
{
   ServiceTorrent torrent;
   getServiceTorrent(slot, &torrent);

   char text[128], speed[24];
   int offset = appendUint64(text, sizeof text, 0, (uint64_t)getDonePercent(&torrent));

   appendStr(text, sizeof text, &offset, "%");
   text[offset] = 0;
   setLabelText(&ringLabels[row], text);
   setLabelText(&nameLabels[row], torrent.name);

   // one that never got as far as knowing its own pieces has only its status to show, and why
   if (torrent.pieceCount <= 0) {
      offset = 0;
      appendStatus(text, sizeof text, &offset, &torrent);
      text[offset] = 0;
      setLabelText(&statLabels[row], text);
      setLabelText(&sizeLabels[row], "");
      return;
   }

   formatSize((uint64_t)torrent.bytesPerSecond, speed);
   int speedLength = getStrLen(speed);
   appendStr(speed, sizeof speed, &speedLength, "/s");
   speed[speedLength] = 0;
   setLabelText(&downSpeedLabels[row], speed);
   setLabelText(&upSpeedLabels[row], "0 B/s");   // nothing is served back yet

   writeStatLine(text, sizeof text, &torrent);
   setLabelText(&statLabels[row], isCompleted(&torrent) ? "" : text);

   char size[24];
   formatSize((uint64_t)torrent.totalLength, size);
   setLabelText(&sizeLabels[row], torrent.totalLength > 0 ? size : "");
}

static void drawTorrentRow(int row, int slot, int isSelected)
{
   ServiceTorrent torrent;
   getServiceTorrent(slot, &torrent);

   int y = getRowY(row);
   int percent = getDonePercent(&torrent);
   int isKnown = torrent.pieceCount > 0;

   drawRowFrame(y, isSelected);

   // section: the ring and the percentage inside it
   drawProgressRing(y + ROW_HEIGHT / 2, percent, getProgressColor(&torrent));
   moveLabel(&ringLabels[row], RING_CENTRE_X - ringLabels[row].tt.tex.w / 2,
             getCenteredLabelY(&ringLabels[row], y, ROW_HEIGHT));
   drawLabel(&ringLabels[row]);

   // section: the name, with the size at the far end of its line once there is nothing under the bar
   int isDone = isCompleted(&torrent);
   int nameY = isKnown ? (isDone ? 34 : 24) : 34;

   moveLabel(&nameLabels[row], TEXT_X, y + nameY);
   drawLabel(&nameLabels[row]);

   if (!isKnown) {
      moveLabel(&statLabels[row], TEXT_X, y + 70);
      drawLabel(&statLabels[row]);
      return;
   }

   // section: the bar, and under it the numbers, which one that is finished has no use for
   int barY = y + (isDone ? 74 : 60);
   fillGfxRectangle(TEXT_X, barY, TEXT_WIDTH, BAR_HEIGHT, COLOR_NEUTRAL_800);
   fillGfxRectangle(TEXT_X, barY, TEXT_WIDTH * percent / 100, BAR_HEIGHT, getProgressColor(&torrent));

   if (!isDone) drawStatRow(row, barY + 24);

   moveLabel(&sizeLabels[row], TEXT_RIGHT - sizeLabels[row].tt.tex.w, y + (isDone ? nameY + 4 : 84));
   drawLabel(&sizeLabels[row]);
}

static void fillResultRow(int row, int index)
{
   ServiceResult result;
   getServiceResult(index, &result);

   char text[128];
   int offset = appendUint64(text, sizeof text, 0, (uint64_t)result.seeders);

   appendStr(text, sizeof text, &offset, " seeding, ");
   offset = appendUint64(text, sizeof text, offset, (uint64_t)result.leechers);
   appendStr(text, sizeof text, &offset, " leeching, from ");
   appendStr(text, sizeof text, &offset, result.sourceName);
   if (index == addedResult) appendStr(text, sizeof text, &offset, "   (added)");
   text[offset] = 0;

   setLabelText(&nameLabels[row], result.title);
   setLabelText(&statLabels[row], text);
   setLabelText(&sizeLabels[row], result.size);
}

static void drawResultRow(int row, int index, int isSelected)
{
   (void)index;

   int y = getRowY(row);
   drawRowFrame(y, isSelected);

   moveLabel(&nameLabels[row], TILE_X + 20, y + 30);
   drawLabel(&nameLabels[row]);

   moveLabel(&statLabels[row], TILE_X + 20, y + 70);
   drawLabel(&statLabels[row]);

   moveLabel(&sizeLabels[row], TEXT_RIGHT - sizeLabels[row].tt.tex.w, y + 70);
   drawLabel(&sizeLabels[row]);
}

// section: what the buttons do

static void onQueryTyped(const char *text)
{
   if (!text || !text[0]) return;   // nothing typed, so there is nothing to look for

   strCopy(query, sizeof query, text);
   searchRan = 1;
   selected[VIEW_SEARCH] = 0;
   scrollTop[VIEW_SEARCH] = 0;
   addedResult = -1;
   searchServiceSources(query);

   // the results are what was asked for, so the view appears and the pad goes to it
   setChromeSearchShown(1);
   showChromeView(VIEW_SEARCH);
}

static int getSelectedSlot(AppView view)
{
   int slots[TORRENT_SLOT_MAX];
   int count = listView(view, slots, TORRENT_SLOT_MAX);

   return selected[view] < count ? slots[selected[view]] : -1;
}

static void toggleSelected(AppView view)
{
   int slot = getSelectedSlot(view);
   if (slot < 0) return;

   ServiceTorrent torrent;
   getServiceTorrent(slot, &torrent);

   if (torrent.status == TORRENT_PAUSED) resumeServiceTorrent(slot);
   else if (torrent.status != TORRENT_FINISHED) pauseServiceTorrent(slot);
}

// the torrent a search result turned into arrives a moment later, at the end of the list
static void standOnTheNewTorrent(void)
{
   if (downloadsBeforeAdd < 0) return;

   int count = getRowCount(VIEW_DOWNLOADS);
   if (count <= downloadsBeforeAdd) return;

   selected[VIEW_DOWNLOADS] = count - 1;
   downloadsBeforeAdd = -1;
}

// section: deleting, which asks first because one of the answers cannot be undone

static void onDeleteAnswer(Answer answer)
{
   if (answer == ANSWER_CIRCLE || slotToDelete < 0) return;

   removeServiceTorrent(slotToDelete, answer == ANSWER_SQUARE);
   slotToDelete = -1;
}

static void askAboutDeleting(AppView view)
{
   slotToDelete = getSelectedSlot(view);
   if (slotToDelete < 0) return;

   ServiceTorrent torrent;
   getServiceTorrent(slotToDelete, &torrent);

   char title[TORRENT_NAME_MAX + 16];
   int offset = 0;

   appendStr(title, sizeof title, &offset, "Delete ");
   appendStr(title, sizeof title, &offset, torrent.name);
   appendStr(title, sizeof title, &offset, "?");
   title[offset] = 0;

   ask(title, "From the app", "From the app and the disk", "Cancel", onDeleteAnswer);
}

static void keepSelectionInView(AppView view)
{
   int count = getRowCount(view);

   if (selected[view] >= count) selected[view] = count - 1;
   if (selected[view] < 0) selected[view] = 0;

   if (selected[view] < scrollTop[view]) scrollTop[view] = selected[view];
   if (selected[view] >= scrollTop[view] + ROWS_VISIBLE) scrollTop[view] = selected[view] - ROWS_VISIBLE + 1;
   if (scrollTop[view] < 0) scrollTop[view] = 0;
}

// section: the screen

static void initHome(void)
{
   font = openSystemFont(FONT_POP);
   initChrome(&font);
   initAskOverlay(&font);   // after the chrome, which is what loads the button pictures


   initLabel(&emptyLabel, &font, CONTENT_X + 26, ROW_TOP + 20, CONTENT_WIDTH - 52, AUTO, TITLE_SIZE, TEXT_FAINT,
             TEXT_NOWRAP, "");

   for (int row = 0; row < ROWS_VISIBLE; row++) {
      initLabelRaw(&nameLabels[row], &font, TEXT_X, 0, TEXT_WIDTH - 200, AUTO, TITLE_SIZE, TEXT_BRIGHT,
                   TEXT_NOWRAP_ELLIPSIS, "");
      initLabelRaw(&statLabels[row], &font, TEXT_X, 0, TEXT_WIDTH - 200, AUTO, LINE_SIZE, TEXT_QUIET,
                   TEXT_NOWRAP_ELLIPSIS, "");
      initLabelRaw(&sizeLabels[row], &font, 0, 0, 180, AUTO, LINE_SIZE, TEXT_PLAIN, TEXT_NOWRAP, "");
      initLabelRaw(&ringLabels[row], &font, 0, 0, 60, AUTO, RING_TEXT_SIZE, TEXT_BRIGHT, TEXT_NOWRAP, "");
      initLabelRaw(&downSpeedLabels[row], &font, 0, 0, 140, AUTO, LINE_SIZE, TEXT_QUIET, TEXT_NOWRAP, "");
      initLabelRaw(&upSpeedLabels[row], &font, 0, 0, 140, AUTO, LINE_SIZE, TEXT_QUIET, TEXT_NOWRAP, "");
   }

   initIcon(&downIcon, ICON_DOWN_BIG, ARROW_SIZE);
   initIcon(&upIcon, ICON_UP_BIG, ARROW_SIZE);

   for (int line = 0; line < SETTINGS_LINES; line++) {
      int y = ROW_TOP + 18 + line * SETTINGS_STEP;

      initLabelRaw(&settingKeyLabels[line], &font, SETTINGS_KEY_X, y, 220, AUTO, LINE_SIZE, TEXT_QUIET, TEXT_NOWRAP,
                   SETTING_NAMES[line]);
      initLabelRaw(&settingValueLabels[line], &font, SETTINGS_VALUE_X, y, CONTENT_WIDTH - 280, AUTO, LINE_SIZE,
                   TEXT_BRIGHT, TEXT_NOWRAP_ELLIPSIS, "");
   }

   for (int line = 0; line < LOG_LINES_VISIBLE; line++)
      initLabelRaw(&logLabels[line], &font, CONTENT_X + 20, ROW_TOP + 14 + line * LOG_LINE_HEIGHT, CONTENT_WIDTH - 40,
                   AUTO, LOG_TEXT_SIZE, TEXT_PLAIN, TEXT_NOWRAP_ELLIPSIS, "");

}

static void resumeHome(void) {}

// The footer only offers what the pad could actually do right now: a button that would do nothing
// to whatever is highlighted is left out rather than shown and ignored.
static void reportWhatButtonsDo(AppView view)
{
   int onTorrent = getChromeFocus() == FOCUS_LIST && view != VIEW_SEARCH;
   int slot = onTorrent ? getSelectedSlot(view) : -1;

   setChromeDeleteShown(slot >= 0);
   if (slot < 0) {
      setChromeActionShown(0);
      return;
   }

   ServiceTorrent torrent;
   getServiceTorrent(slot, &torrent);

   // one that has finished has nothing to start or stop
   setChromeActionShown(!isCompleted(&torrent));
   setChromeActionHint(torrent.status == TORRENT_PAUSED ? "Start" : "Stop");
}

// The first three are shown to be read, not changed; only the last two come from settings.txt.
static void fillSettings(void)
{
   setLabelText(&settingValueLabels[0], getDownloadsPath());

   char text[256], sourceNames[160];
   int offset = 0;

   appendStr(text, sizeof text, &offset, getSourcesPath());
   getServiceSourceNames(sourceNames, sizeof sourceNames);

   if (sourceNames[0]) {
      appendStr(text, sizeof text, &offset, "   (");
      appendStr(text, sizeof text, &offset, sourceNames);
      appendStr(text, sizeof text, &offset, ")");
   }

   text[offset] = 0;
   setLabelText(&settingValueLabels[1], text);
   setLabelText(&settingValueLabels[2], getWgConfigPath());

   const char *vpnMode = "Off";
   if (isVpnEnabled())
      vpnMode = isKillSwitchOn() ? "On, nothing goes out while it is down"
                                 : "On, falls back to the console while it is down";

   setLabelText(&settingValueLabels[3], vpnMode);
   setLabelText(&settingValueLabels[4], isSecureDeleteOn() ? "On" : "Off");
}

// Every piece of text the next frame will show, set before the frame starts. Nothing here draws.
static void fillLabels(AppView view)
{
   if (view == VIEW_SETTINGS) { fillSettings(); return; }

   if (view == VIEW_LOGS) {
      int count = getLogLineCount();
      int shown = count < LOG_LINES_VISIBLE ? count : LOG_LINES_VISIBLE;
      int first = count - shown;
      if (first < 0) first = 0;

      char line[LOG_LINE_MAX];
      for (int index = 0; index < LOG_LINES_VISIBLE; index++) {
         if (index < shown) getLogLine(first + index, line, sizeof line);
         else line[0] = 0;

         setLabelText(&logLabels[index], line);
      }

      return;
   }

   int count = getRowCount(view);
   if (count == 0) {
      const char *text = "Nothing here. Press Triangle to search for something.";

      if (getServiceStatus() == SERVICE_FAILED) text = getServiceMessage();
      else if (view == VIEW_COMPLETED) text = "Nothing downloaded yet.";
      else if (view == VIEW_SEARCH)
         text = isServiceSearching() ? "Searching..."
                                     : searchRan ? "Nothing found. Press Triangle to try something else."
                                                 : "Press Triangle to search.";

      setLabelText(&emptyLabel, text);
      return;
   }

   int slots[TORRENT_SLOT_MAX];
   int slotCount = view == VIEW_SEARCH ? 0 : listView(view, slots, TORRENT_SLOT_MAX);

   for (int row = 0; row < ROWS_VISIBLE && scrollTop[view] + row < count; row++) {
      int index = scrollTop[view] + row;

      if (view == VIEW_SEARCH) fillResultRow(row, index);
      else if (index < slotCount) fillTorrentRow(row, slots[index]);
   }
}

static void updateHome(void)
{
   if (oskInputActive()) return;

   // a question on the screen takes the pad until it is answered
   if (isOverlayVisible(&askOverlay)) {
      updateOverlay(&askOverlay);
      return;
   }

   AppFocus focusBefore = getChromeFocus();
   AppView view = updateChrome();
   int count = getRowCount(view);

   // the press that moved the pad into the list must not also act on the row it landed on
   int justEnteredList = focusBefore == FOCUS_SIDEBAR && getChromeFocus() == FOCUS_LIST;

   // an empty panel has nothing to stand on, so the pad goes back to the list of views
   if (count == 0) setChromeFocus(FOCUS_SIDEBAR);

   // searching goes out over the tunnel like everything else, so there is no point asking without one
   if (isPadButtonPressed(PAD_BTN_TRIANGLE) && getServiceStatus() != SERVICE_FAILED)
      oskInputBegin("What to search for", "", onQueryTyped);

   if (getChromeFocus() == FOCUS_LIST) {
      if (isRepeatDue(&downRepeat, getPadButtonState(PAD_BTN_DOWN)) && selected[view] + 1 < count) selected[view]++;
      if (isRepeatDue(&upRepeat, getPadButtonState(PAD_BTN_UP)) && selected[view] > 0) selected[view]--;

      // a result that was added belongs in the downloads, standing on the row it will appear as
      if (count > 0 && view == VIEW_SEARCH && !justEnteredList && isPadButtonPressed(PAD_BTN_CROSS)) {
         addServiceResult(selected[view]);
         addedResult = selected[view];
         downloadsBeforeAdd = getRowCount(VIEW_DOWNLOADS);
         showChromeView(VIEW_DOWNLOADS);
      }

      if (count > 0 && view != VIEW_SEARCH) {
         if (isPadButtonPressed(PAD_BTN_SQUARE)) toggleSelected(view);
         if (isPadButtonPressed(PAD_BTN_CIRCLE)) askAboutDeleting(view);
      }
   }

   standOnTheNewTorrent();
   keepSelectionInView(view);
   reportWhatButtonsDo(view);
   fillLabels(view);
}

// what the sidebar shows about the whole app
static void reportTotals(void)
{
   int downloading = 0, completed = 0;

   for (int slot = 0; slot < getServiceTorrentCount(); slot++) {
      ServiceTorrent torrent;
      getServiceTorrent(slot, &torrent);

      if (isCompleted(&torrent)) completed++;
      else downloading++;
   }

   ServiceStatus status = getServiceStatus();
   setChromeCounts(downloading, completed);
   setChromeVpn(getServiceAddress(), status == SERVICE_FAILED, isServiceUsingTunnel());
}

static void drawSettings(void)
{
   for (int line = 0; line < SETTINGS_LINES; line++) {
      drawLabel(&settingKeyLabels[line]);
      drawLabel(&settingValueLabels[line]);
   }
}

static void drawLogs(void)
{
   for (int line = 0; line < LOG_LINES_VISIBLE; line++) drawLabel(&logLabels[line]);
}

// the one line a view shows when it has no rows, in the middle of the empty panel
static void drawEmptyMessage(void)
{
   moveLabel(&emptyLabel, CONTENT_X + (CONTENT_WIDTH - emptyLabel.tt.tex.w) / 2,
             (CONTENT_TOP + PANEL_BOTTOM - emptyLabel.tt.tex.h) / 2);
   drawLabel(&emptyLabel);
}

static void drawRows(AppView view, int count)
{
   int slots[TORRENT_SLOT_MAX];
   int slotCount = view == VIEW_SEARCH ? 0 : listView(view, slots, TORRENT_SLOT_MAX);

   for (int row = 0; row < ROWS_VISIBLE && scrollTop[view] + row < count; row++) {
      int index = scrollTop[view] + row;
      int isSelected = index == selected[view];

      if (view == VIEW_SEARCH) drawResultRow(row, index, isSelected);
      else if (index < slotCount) drawTorrentRow(row, slots[index], isSelected);
   }
}

static void drawHome(void)
{
   reportTotals();

   AppView view = getChromeView();
   int count = getRowCount(view);

   drawChrome();

   if (view == VIEW_SETTINGS) drawSettings();
   else if (view == VIEW_LOGS) drawLogs();
   else if (count == 0) drawEmptyMessage();
   else drawRows(view, count);

   drawOverlay(&askOverlay);
}

static void suspendHome(void) {}

static void termHome(void)
{
   for (int row = 0; row < ROWS_VISIBLE; row++) {
      freeLabel(&nameLabels[row]);
      freeLabel(&statLabels[row]);
      freeLabel(&sizeLabels[row]);
      freeLabel(&ringLabels[row]);
      freeLabel(&downSpeedLabels[row]);
      freeLabel(&upSpeedLabels[row]);
   }

   for (int line = 0; line < SETTINGS_LINES; line++) {
      freeLabel(&settingKeyLabels[line]);
      freeLabel(&settingValueLabels[line]);
   }
   for (int line = 0; line < LOG_LINES_VISIBLE; line++) freeLabel(&logLabels[line]);

   termOverlay(&askOverlay);
   freeLabel(&emptyLabel);
   termChrome();
   closeFont(&font);
}

Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
