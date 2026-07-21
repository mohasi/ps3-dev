#include "screens/home.h"
#include "gfx.h"
#include "audio.h"
#include "font.h"
#include "ftp.h"
#include "ui/icon-font.h"
#include "ui/label.h"
#include "ui/console-glyphs.h"
#include "ui/breadcrumb.h"
#include "ui/keyboard.h"
#include "ui/hex-pad.h"
#include "widgets/clock-widget.h"
#include "widgets/footer-widget.h"
#include "widgets/free-space-widget.h"
#include "widgets/file-list.h"
#include "search-controller.h"
#include "overlays/sidepanel.h"
#include "overlays/confirm-overlay.h"
#include "overlays/progress-overlay.h"
#include "overlays/image-viewer-overlay.h"
#include "overlays/audio-player-overlay.h"
#include "overlays/video-player-overlay.h"
#include "overlays/text-editor-overlay.h"
#include "overlays/hex-viewer-overlay.h"
#include "overlays/properties-overlay.h"
#include "file-actions.h"
#include "theme.h"
#include "pad.h"
#include "string-utilities.h"   // appendStr / appendUint64 / formatIpv4 for the FTP button label
#include "network.h"

static Font pop;
static Icon  titleFolderIcon;
static Icon  clockIcon;
static Label titleLabel;
static Breadcrumb breadcrumb;
static Audio clickSfx;
static Audio checkSfx;

// the whole screen background is a solid fill (no background.png) with the header/footer chrome
// drawn on top, so everything here can be moved, restyled, and themed freely
#define BOX_X          44   // breadcrumb container (was a rounded sprite); now a flat metro box
#define BOX_Y         117
#define BOX_W        1832
#define BOX_H          56
#define TITLE_ICON_X   40
#define TITLE_ICON_Y   28
#define TITLE_ICON_SIZE 56
#define TITLE_ICON_COLOR 0xFF2F6FD0   // brand blue for the open-folder logo, fixed across themes
#define TITLE_TEXT_X  136
#define TITLE_TEXT_Y   44
#define TITLE_TEXT_SIZE 32
#define CLOCK_ICON_X  1848   // where the old baked clock icon sat, top-right
#define CLOCK_ICON_Y    47
#define CLOCK_ICON_H    37   // the clock glyph is drawn in a CLOCK_ICON_H square box
#define DIVIDER_X      51
#define DIVIDER_W    (1869 - 51)
#define DIVIDER_TOP_Y  932
#define DIVIDER_BOT_Y  988

// Sets the SELECT button label to "<prefix> / <ip>:<port>", or just "<prefix>"
// if the local IP can't be resolved.
static void setFtpButtonLabel(const char *prefix)
{
   char label[48];
   int length = 0;
   appendStr(label, sizeof label, &length, prefix);

   uint32_t ip;
   if (getLocalIpv4(&ip) == 0) {
      appendStr(label, sizeof label, &length, " / ");
      length += formatIpv4(label + length, (int)sizeof label - length, ip);
      if (length < (int)sizeof label - 1) label[length++] = ':';
      length = appendUint64(label, sizeof label, length, FTP_DEFAULT_PORT);
   }
   label[length] = 0;
   setFooterButtonText(PAD_BTN_SELECT, label);
}

static void toggleFtpServer(void)
{
   if (isFtpServerRunning() == FTP_STARTED) {
      stopFtpServer();
      setFooterButtonText(PAD_BTN_SELECT, "Start FTP Server");
      return;
   }

   if (startFtpServer(FTP_DEFAULT_PORT) != FTP_OK) return;
   setFtpButtonLabel("Stop FTP Server");
}

// the shared sidepanel has one handler; route it by which list is showing (see search-controller).
static void onSidepanelAction(SelectionAction action)
{
   if (isSearchActive()) dispatchSearchAction(action);
   else                  dispatchAction(action);
}

static void openSidepanel(void)
{
   int count;
   const SelectionAction *actions;
   const SelectionSummary *summary;
   if (isSearchActive()) {
      if (!searchHasOptions()) return;   // no results yet, or the walk is still running
      actions = getSearchActions(&count);
      summary = getSearchSummary();
   } else {
      actions = getAvailableActions(&count);
      summary = getSelectionSummary();
   }
   setSidepanelContent(summary, actions, count);
   showOverlay(&sidepanel);
}

// the on-screen keyboard / hex-pad palette, from the active theme (the docked slab uses the side-menu
// colours; the selected key uses the row-highlight colours).
static KeyGridTheme makeKeyGridTheme(void)
{
   KeyGridTheme theme;
   theme.panelFill          = activeTheme->menuFill;
   theme.panelBorder        = activeTheme->menuBorder;
   theme.keyHighlightFill   = activeTheme->highlightFill;
   theme.keyHighlightBorder = activeTheme->highlightBorder;
   theme.keyText            = activeTheme->textPrimary;
   theme.borderThickness    = activeTheme->borderThickness;
   return theme;
}

static void initHome(void)
{
   pop      = openSystemFont(FONT_POP);
   clickSfx = loadAudio("/dev_hdd0/game/FILEMGR01/USRDIR/click.wav", AUDIO_MEMORY);
   checkSfx = loadAudio("/dev_hdd0/game/FILEMGR01/USRDIR/check.wav", AUDIO_MEMORY);

   initIcon(&titleFolderIcon, ICON_FOLDER_OPEN, TITLE_ICON_SIZE);
   initIcon(&clockIcon, ICON_CLOCK, CLOCK_ICON_H);
   initLabel(&titleLabel, &pop, TITLE_TEXT_X, TITLE_TEXT_Y, AUTO, AUTO, TITLE_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "PS3 File Manager");
   initBreadcrumb(&breadcrumb, &pop, 70, 135, activeTheme->textPrimary, 20);
   initClockWidget(&pop, 1675, 55, 21);
   initFreeSpaceWidget(&pop, 1668, 951, 20, 210);
   initFooterWidget(&pop);
   initFileList(&pop, &clickSfx, &checkSfx, 177, 258, 860, 74, 24, &breadcrumb);   // name width leaves room for the permissions column
   initSearchController(&pop, &clickSfx, &checkSfx, 258, 74, 24, openSidepanel);
   initSidepanel(&clickSfx, onSidepanelAction);
   initConfirmOverlay(&clickSfx);
   initPropertiesOverlay(&clickSfx);
   initProgressOverlay(&clickSfx);
   initAudioPlayerOverlay();
   initVideoPlayerOverlay();
   initTextEditorOverlay();
   initHexViewerOverlay();
   KeyGridTheme keyGridTheme = makeKeyGridTheme();
   initKeyboard(keyGridTheme);
   initHexPad(keyGridTheme);
   addFooterButton(PAD_BTN_TRIANGLE, GLYPH_TRIANGLE, "Options", openSidepanel);
   addFooterButton(PAD_BTN_R1, GLYPH_R1, "Theme", NULL);   // hint only; handleThemeSwitch owns the R1 press
   addFooterButton(PAD_BTN_START, GLYPH_START, "Search", launchSearch);
   addFooterButton(PAD_BTN_SELECT, GLYPH_SELECT, "Start FTP Server", toggleFtpServer);

   if (!isNetworkAvailable()) {
      // No usable network: there is nothing to serve, so disable the toggle.
      setFooterButtonText(PAD_BTN_SELECT, "FTP Server / No Network");
      setFooterButtonEnabled(PAD_BTN_SELECT, 0);
   } else if (!isFtpPortAvailable(FTP_DEFAULT_PORT)) {
      // Another FTP server already owns the port: show where it's reachable
      // but leave the button disabled — we don't own it and can't toggle it.
      setFtpButtonLabel("FTP Server");
      setFooterButtonEnabled(PAD_BTN_SELECT, 0);
   }
}

static void resumeHome(void) {}

// pushes the active theme into every persistent label/checkbox on the home screen. the chrome drawn
// live from activeTheme (background, boxes, highlights, separators, bars) re-themes for free; this
// covers only the pre-rendered pieces that captured a colour at init.
static void applyThemeToHome(void)
{
   setLabelColor(&titleLabel, activeTheme->textPrimary);
   rethemeBreadcrumb(&breadcrumb, activeTheme->textPrimary);
   rethemeClockWidget();
   rethemeFreeSpaceWidget();
   rethemeFileList();
   rethemeSearchController();
   rethemeFooterWidget();
   KeyGridTheme keyGridTheme = makeKeyGridTheme();
   rethemeKeyboard(keyGridTheme);
   rethemeHexPad(keyGridTheme);
   rethemeTextEditorOverlay();
   rethemeHexViewerOverlay();
   rethemeSidepanel();
   rethemeConfirmOverlay();
   rethemePropertiesOverlay();
   rethemeProgressOverlay();
}

// R1 cycles to the next theme (wrapping) and applies it instantly. modal overlays and the on-screen
// keyboard/hex-pad own R1 themselves, so this only runs on the bare home screen.
static void handleThemeSwitch(void)
{
   int count = getThemeCount();
   if (count < 2) return;
   if (!isPadButtonPressed(PAD_BTN_R1)) return;

   setActiveThemeIndex((getActiveThemeIndex() + 1) % count);
   applyThemeToHome();
   playAudioOnce(&clickSfx);
}

static inline int anyOverlayVisible(void)
{
   return isOverlayVisible(&sidepanel)
      || isOverlayVisible(&confirmOverlay)
      || isOverlayVisible(&progressOverlay)
      || isOverlayVisible(&imageViewerOverlay)
      || isOverlayVisible(&audioPlayerOverlay)
      || isOverlayVisible(&videoPlayerOverlay)
      || isOverlayVisible(&textEditorOverlay)
      || isOverlayVisible(&hexViewerOverlay)
      || isOverlayVisible(&propertiesOverlay);
}

static void updateHome(void)
{
   // snapshot before updating overlays: an overlay that closes this frame
   // (e.g. confirm on cross) must not let that same press fall through to the
   // file list, so the world stays paused for the frame the overlay was up.
   int overlayWasVisible = anyOverlayVisible();

   updateOverlay(&sidepanel);
   updateOverlay(&confirmOverlay);
   updateOverlay(&progressOverlay);
   updateOverlay(&imageViewerOverlay);
   updateOverlay(&audioPlayerOverlay);
   updateOverlay(&videoPlayerOverlay);
   updateOverlay(&textEditorOverlay);
   updateOverlay(&hexViewerOverlay);
   updateOverlay(&propertiesOverlay);
   updateKeyboard();
   updateHexPad();

   if (!overlayWasVisible) {
      if (!isKeyboardOpen() && !isHexPadOpen()) handleThemeSwitch();
      updateClockWidget();
      setFreeSpacePath(getCurrentPath());   // report the volume the user is in
      updateFreeSpaceWidget();
      if (isSearchActive()) {
         updateSearchView();
      } else if (handleSearchBack()) {
         // Back at the jumped-into folder re-entered the search; nothing else this frame
      } else {
         updateFooterWidget();
         updateFileList();
      }
   }
}

static void drawHome(void)
{
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), activeTheme->appBg);
   drawGfxBox(BOX_X, BOX_Y, BOX_W, BOX_H, activeTheme->borderThickness, activeTheme->panelFill, activeTheme->panelBorder);
   drawIcon(&titleFolderIcon, TITLE_ICON_X, TITLE_ICON_Y, TITLE_ICON_COLOR);
   drawIcon(&clockIcon, CLOCK_ICON_X, CLOCK_ICON_Y, activeTheme->textPrimary);
   drawLabel(&titleLabel);
   fillGfxRectangle(DIVIDER_X, DIVIDER_TOP_Y, DIVIDER_W, 2, activeTheme->divider);
   fillGfxRectangle(DIVIDER_X, DIVIDER_BOT_Y, DIVIDER_W, 2, activeTheme->divider);
   if (isSearchActive()) drawSearchTitle(); else drawBreadcrumb(&breadcrumb);
   drawClockWidget();
   drawFreeSpaceWidget();
   if (isSearchActive()) drawSearchResults(); else drawFileList();
   drawFooterWidget();
   drawOverlay(&imageViewerOverlay);
   drawOverlay(&audioPlayerOverlay);
   drawOverlay(&videoPlayerOverlay);
   drawOverlay(&textEditorOverlay);
   drawOverlay(&hexViewerOverlay);
   drawKeyboard();
   drawHexPad();
   drawOverlay(&sidepanel);
   drawOverlay(&propertiesOverlay);
   drawOverlay(&confirmOverlay);
   drawOverlay(&progressOverlay);
}

static void suspendHome(void) {}

static void termHome(void)
{
   stopFtpServer();
   termKeyboard();
   termHexPad();
   termOverlay(&propertiesOverlay);
   termOverlay(&hexViewerOverlay);
   termOverlay(&textEditorOverlay);
   termOverlay(&videoPlayerOverlay);
   termOverlay(&audioPlayerOverlay);
   termOverlay(&imageViewerOverlay);
   termOverlay(&progressOverlay);
   termOverlay(&confirmOverlay);
   termOverlay(&sidepanel);
   termFileList();
   termSearchController();
   termFooterWidget();
   termClockWidget();
   termFreeSpaceWidget();
   termBreadcrumb(&breadcrumb);
   freeLabel(&titleLabel);

   freeAudio(&clickSfx);
   freeAudio(&checkSfx);
   closeFont(&pop);
}

Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
