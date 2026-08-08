// chrome - the frame every view sits in.

#include "widgets/chrome.h"

#include <cell/rtc.h>

#include "dbg.h"
#include "format.h"
#include "gfx.h"
#include "pad.h"
#include "string-utilities.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "ui/icon-font.h"
#include "ui/label.h"
#include "widgets/theme.h"

#define SIDEBAR_X      45
#define SIDEBAR_WIDTH 365
#define ITEM_X        (SIDEBAR_X + 12)
#define ITEM_WIDTH    (SIDEBAR_WIDTH - 24)
#define ITEM_HEIGHT    56
#define ITEM_STEP      68
#define ITEM_TOP      140

// the tunnel's own line sits against the bottom of the sidebar
#define STATUS_TEXT_X  (SIDEBAR_X + 48)
#define VPN_RULE       (PANEL_BOTTOM - 62)
#define VPN_TOP        (PANEL_BOTTOM - 40)
#define HINTS_X        SIDEBAR_X
#define HINTS_Y       1020
#define HINTS_GAP       48

#define ICON_SIZE      26
#define SMALL_ICON     20
#define TEXT_SIZE      24
#define SMALL_SIZE     20
#define VPN_SIZE       17
#define VPN_ICON       22

static Font *font;
static AppView view;
static AppFocus focus;

static Label titleLabel;
static Label subtitleLabel;
static Label clockLabel;
static Label itemLabels[VIEW_COUNT];
static Label vpnLabel;
static Icon itemIcons[VIEW_COUNT];
static Icon clockIcon;
static Icon lockIcon;
static ButtonHints hints;
static GfxTexture logo;

static int isTunnelUp;

static const IconId ITEM_ICONS[VIEW_COUNT] = { ICON_DOWNLOAD, ICON_CHECK, ICON_SEARCH, ICON_DOC_TEXT, ICON_COG };
static const char *ITEM_NAMES[VIEW_COUNT] = { "Downloads", "Completed", "Search Results", "Logs", "Settings" };

static int searchShown;   // the results have nowhere to come from until a search has run
static int squareHint;    // the one hint whose wording changes with what is picked
static int deleteHint;

static int isViewShown(AppView view)
{
   return view != VIEW_SEARCH || searchShown;
}

// where an item sits, counting only the ones on show
static int getItemY(AppView view)
{
   int place = 0;
   for (int index = 0; index < view; index++)
      if (isViewShown(index)) place++;

   return ITEM_TOP + place * ITEM_STEP;
}

// the next view up or down the list, skipping any that is not on show
static AppView getNeighbourView(AppView view, int step)
{
   for (int next = view + step; next >= 0 && next < VIEW_COUNT; next += step)
      if (isViewShown(next)) return next;

   return view;
}

// the app's icon with its empty margins already cut off, shipped beside the app
#define LOGO_PATH   "/dev_hdd0/game/SWARM0001/USRDIR/logo.png"
#define LOGO_X      36
#define LOGO_Y      20
#define LOGO_WIDTH  80
#define LOGO_HEIGHT 76

static void drawLogo(void)
{
   if (!logo.offset) return;
   drawGfxTexture(LOGO_X, LOGO_Y, LOGO_WIDTH, LOGO_HEIGHT, logo, 0, 0, 1, 1, COLOR_WHITE, GFX_FILTER_LINEAR);
}

static void updateClock(void)
{
   CellRtcDateTime now;
   if (cellRtcGetCurrentClockLocalTime(&now) != 0) return;

   char text[24];
   int offset = 0;

   if (now.day < 10) text[offset++] = '0';
   offset = appendUint64(text, sizeof text, offset, now.day);
   text[offset++] = '/';
   if (now.month < 10) text[offset++] = '0';
   offset = appendUint64(text, sizeof text, offset, now.month);
   text[offset++] = ' ';
   text[offset++] = ' ';
   if (now.hour < 10) text[offset++] = '0';
   offset = appendUint64(text, sizeof text, offset, now.hour);
   text[offset++] = ':';
   if (now.minute < 10) text[offset++] = '0';
   offset = appendUint64(text, sizeof text, offset, now.minute);
   text[offset] = 0;

   setLabelText(&clockLabel, text);
}

void initChrome(Font *appFont)
{
   font = appFont;
   initIconFont();
   loadConsoleGlyphs();

   logo = loadGfxTexture(LOGO_PATH);
   if (!logo.offset) logError("[swarm] chrome: " LOGO_PATH " could not be read\n");

   initLabel(&titleLabel, font, 145, 26, 400, AUTO, 36, TEXT_BRIGHT, TEXT_NOWRAP, "Swarm");
   initLabel(&subtitleLabel, font, 147, 70, 400, AUTO, SMALL_SIZE, TEXT_QUIET, TEXT_NOWRAP, "Torrent Client");
   initLabel(&clockLabel, font, 1665, 42, 220, AUTO, TEXT_SIZE, TEXT_PLAIN, TEXT_NOWRAP, "");
   initIcon(&clockIcon, ICON_CLOCK, ICON_SIZE);
   initIcon(&lockIcon, ICON_LOCK, VPN_ICON);

   for (int index = 0; index < VIEW_COUNT; index++) {
      initLabel(&itemLabels[index], font, SIDEBAR_X + 74, 0, 260, AUTO, TEXT_SIZE, TEXT_PLAIN, TEXT_NOWRAP,
                ITEM_NAMES[index]);
      initIcon(&itemIcons[index], ITEM_ICONS[index], ICON_SIZE);
   }

   initLabelRaw(&vpnLabel, font, STATUS_TEXT_X, VPN_TOP, 300, AUTO, VPN_SIZE, COLOR_AMBER_400, TEXT_NOWRAP, "");

   initButtonHints(&hints, font, HINTS_Y, 30, TEXT_SIZE, TEXT_PLAIN);
   setButtonHintGap(&hints, HINTS_GAP);
   squareHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), "Stop");
   deleteHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE), "Delete");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Search");
}

void termChrome(void)
{
   for (int index = 0; index < VIEW_COUNT; index++) freeLabel(&itemLabels[index]);

   termButtonHints(&hints);
   freeLabel(&vpnLabel);
   freeLabel(&clockLabel);
   freeLabel(&subtitleLabel);
   freeLabel(&titleLabel);
   freeGfxTexture(&logo);
   freeConsoleGlyphs();
   freeIconFont();
}

AppView updateChrome(void)
{
   // the logs and the settings are read, not worked on, so the pad stays on the list of views
   int hasRows = view != VIEW_LOGS && view != VIEW_SETTINGS;

   if (focus == FOCUS_SIDEBAR) {
      if (hasRows && (isPadButtonPressed(PAD_BTN_RIGHT) || isPadButtonPressed(PAD_BTN_CROSS))) focus = FOCUS_LIST;
      if (isPadButtonPressed(PAD_BTN_DOWN)) view = getNeighbourView(view, 1);
      if (isPadButtonPressed(PAD_BTN_UP)) view = getNeighbourView(view, -1);
   } else if (isPadButtonPressed(PAD_BTN_LEFT) || !hasRows) {
      focus = FOCUS_SIDEBAR;
   }

   return view;
}

void setChromeSearchShown(int shown)
{
   searchShown = shown;
   if (!shown && view == VIEW_SEARCH) view = VIEW_DOWNLOADS;
}

void setChromeActionHint(const char *caption)
{
   setButtonHintCaption(&hints, squareHint, caption);
}

void setChromeActionShown(int shown)
{
   setButtonHintShown(&hints, squareHint, shown);
}

void setChromeDeleteShown(int shown)
{
   setButtonHintShown(&hints, deleteHint, shown);
}

AppView getChromeView(void)
{
   return view;
}

void showChromeView(AppView wanted)
{
   view = wanted;
   focus = FOCUS_LIST;
}

AppFocus getChromeFocus(void)
{
   return focus;
}

void setChromeFocus(AppFocus wanted)
{
   focus = wanted;
}

void setChromeCounts(int downloading, int completed)
{
   char text[48];
   int offset = 0;

   appendStr(text, sizeof text, &offset, ITEM_NAMES[VIEW_DOWNLOADS]);
   appendStr(text, sizeof text, &offset, " (");
   offset = appendUint64(text, sizeof text, offset, (uint64_t)downloading);
   appendStr(text, sizeof text, &offset, ")");
   text[offset] = 0;
   setLabelText(&itemLabels[VIEW_DOWNLOADS], text);

   offset = 0;
   appendStr(text, sizeof text, &offset, ITEM_NAMES[VIEW_COMPLETED]);
   appendStr(text, sizeof text, &offset, " (");
   offset = appendUint64(text, sizeof text, offset, (uint64_t)completed);
   appendStr(text, sizeof text, &offset, ")");
   text[offset] = 0;
   setLabelText(&itemLabels[VIEW_COMPLETED], text);
}

void setChromeVpn(const char *address, int failed, int usingTunnel)
{
   char text[64];
   int offset = 0;

   isTunnelUp = usingTunnel && address[0] != 0 && !failed;

   // the address is only worth showing when it is the tunnel's: the console's own is a house address
   if (failed) appendStr(text, sizeof text, &offset, "VPN: OFF  /  NET BLOCKED");
   else if (isTunnelUp) {
      appendStr(text, sizeof text, &offset, "VPN: ON  /  ");
      appendStr(text, sizeof text, &offset, address);
   } else if (usingTunnel) {
      appendStr(text, sizeof text, &offset, "VPN: connecting");
   } else {
      appendStr(text, sizeof text, &offset, "VPN: OFF  /  NOT PROTECTED");
   }

   text[offset] = 0;
   setLabelText(&vpnLabel, text);
   setLabelColor(&vpnLabel, isTunnelUp ? COLOR_EMERALD_400 : (usingTunnel && !failed ? COLOR_AMBER_400
                                                                                     : COLOR_RED_400));
}

static void drawSidebarItems(void)
{
   for (int index = 0; index < VIEW_COUNT; index++) {
      if (!isViewShown(index)) continue;

      int y = getItemY(index);
      int isCurrent = index == view;
      int isPicked = isCurrent && focus == FOCUS_SIDEBAR;

      if (isCurrent)
         drawGfxBox(ITEM_X, y, ITEM_WIDTH, ITEM_HEIGHT, 2, isPicked ? PICKED_FILL : PANEL_FILL,
                    isPicked ? PICKED_BORDER : RESTING_BORDER);

      drawIcon(&itemIcons[index], SIDEBAR_X + 30, y + 15, isPicked ? PICKED_BORDER : TEXT_QUIET);
      setLabelColor(&itemLabels[index], isPicked ? PICKED_BORDER : TEXT_PLAIN);
      moveLabel(&itemLabels[index], SIDEBAR_X + 74, y + 15);
      drawLabel(&itemLabels[index]);
   }
}

// the tunnel's one line, padlock and all, centred across the sidebar
static void drawVpnStatus(void)
{
   int width = VPN_ICON + 8 + vpnLabel.tt.tex.w;
   int x = SIDEBAR_X + (SIDEBAR_WIDTH - width) / 2;

   drawGfxLine(SIDEBAR_X + 16, VPN_RULE, SIDEBAR_X + SIDEBAR_WIDTH - 26, VPN_RULE, 1, PANEL_BORDER);
   drawIcon(&lockIcon, x, VPN_TOP - 4, vpnLabel.color);

   moveLabel(&vpnLabel, x + VPN_ICON + 8, VPN_TOP);
   drawLabel(&vpnLabel);
}

void drawChrome(void)
{
   updateClock();

   // section: the header
   drawLogo();
   drawLabel(&titleLabel);
   drawLabel(&subtitleLabel);
   drawIcon(&clockIcon, 1852, 38, TEXT_QUIET);
   drawLabel(&clockLabel);

   // section: the two panels the rest of the screen sits in
   int panelHeight = PANEL_BOTTOM - PANEL_TOP;

   drawGfxBox(SIDEBAR_X, PANEL_TOP, SIDEBAR_WIDTH, panelHeight, 1, PANEL_FILL, PANEL_BORDER);
   drawGfxBox(CONTENT_X, PANEL_TOP, CONTENT_WIDTH, panelHeight, 1, PANEL_FILL, PANEL_BORDER);

   drawSidebarItems();
   drawVpnStatus();

   drawButtonHintsAt(&hints, HINTS_X);
}
