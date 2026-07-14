#include "screens/home.h"
#include "gfx.h"
#include "audio.h"
#include "colors.h"
#include "font.h"
#include "ftp.h"
#include "ui/image.h"
#include "ui/label.h"
#include "ui/slice.h"
#include "ui/console-glyphs.h"
#include "ui/breadcrumb.h"
#include "ui/keyboard.h"
#include "ui/hex-pad.h"
#include "widgets/clock-widget.h"
#include "widgets/footer-widget.h"
#include "widgets/free-space-widget.h"
#include "widgets/file-list.h"
#include "overlays/sidepanel.h"
#include "overlays/confirm-overlay.h"
#include "overlays/progress-overlay.h"
#include "overlays/image-viewer-overlay.h"
#include "overlays/audio-player-overlay.h"
#include "overlays/video-player-overlay.h"
#include "overlays/text-editor-overlay.h"
#include "overlays/hex-viewer-overlay.h"
#include "file-actions.h"
#include "sprite-regions.h"
#include "string-utilities.h"
#include "network.h"

static Font pop;
static GfxTexture sprites;
static Image titleFolderIcon;
static Image clockIcon;
static Label titleLabel;
static NineSlice breadcrumbBox;
static Breadcrumb breadcrumb;
static Audio clickSfx;
static Audio checkSfx;

// the whole screen background is a solid fill (no background.png) with the header/footer chrome
// drawn on top, so everything here can be moved, restyled, and themed freely
#define COLOR_APP_BG   0xFF001636u   // flat navy the old background.png used everywhere
#define BOX_X          44   // breadcrumb container (was baked into the background); now a nine-slice
#define BOX_Y         117
#define BOX_W        1832
#define BOX_H          46
#define BOX_CAP        14   // rounded-corner cap of the breadcrumb-box sprite
#define TITLE_ICON_X   40
#define TITLE_ICON_Y   30
#define TITLE_ICON_W   70
#define TITLE_ICON_H   58
#define TITLE_TEXT_X  136
#define TITLE_TEXT_Y   44
#define TITLE_TEXT_SIZE 32
#define CLOCK_ICON_X  1848   // where the old baked clock icon sat, top-right
#define CLOCK_ICON_Y    47
#define CLOCK_ICON_W    36
#define CLOCK_ICON_H    37
#define DIVIDER_X      51
#define DIVIDER_W    (1869 - 51)
#define DIVIDER_TOP_Y  932
#define DIVIDER_BOT_Y  988
#define COLOR_DIVIDER  0xFF232D43u   // exact colour of the old baked divider lines (bg is solid, so opaque)

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

static void openSidepanel(void)
{
   int count;
   const SelectionAction *actions = getAvailableActions(&count);
   const SelectionSummary *summary = getSelectionSummary();
   setSidepanelContent(summary, actions, count);
   showOverlay(&sidepanel);
}

static void initHome(void)
{
   pop      = openSystemFont(FONT_POP);
   sprites  = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/sprites.png");
   clickSfx = loadAudio("/dev_hdd0/game/FILEMGR01/USRDIR/click.wav", AUDIO_MEMORY);
   checkSfx = loadAudio("/dev_hdd0/game/FILEMGR01/USRDIR/check.wav", AUDIO_MEMORY);

   initNineSlice(&breadcrumbBox, sprites, BOX_X, BOX_Y, BOX_W, BOX_H, spriteRegions[SPRITE_BREADCRUMB_BOX], BOX_CAP, BOX_CAP);
   initImage(&titleFolderIcon, sprites, TITLE_ICON_X, TITLE_ICON_Y, TITLE_ICON_W, TITLE_ICON_H, spriteRegions[SPRITE_TITLE_FOLDER], GFX_FILTER_LINEAR);
   initImage(&clockIcon, sprites, CLOCK_ICON_X, CLOCK_ICON_Y, CLOCK_ICON_W, CLOCK_ICON_H, spriteRegions[SPRITE_CLOCK], GFX_FILTER_LINEAR);
   initLabel(&titleLabel, &pop, TITLE_TEXT_X, TITLE_TEXT_Y, AUTO, AUTO, TITLE_TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, "PS3 File Manager");
   initBreadcrumb(&breadcrumb, &pop, 70, 130, COLOR_WHITE, sprites, spriteRegions[SPRITE_CHEVRON], 20);
   initClockWidget(&pop, 1675, 55, 21, COLOR_WHITE);
   initFreeSpaceWidget(&pop, 1668, 951, 20, 0x64FFFFFF, 210);
   initFooterWidget(&pop);
   initFileList(&pop, sprites, &clickSfx, &checkSfx, 177, 244, 860, 74, 24, COLOR_WHITE, &breadcrumb);   // name width leaves room for the permissions column
   initSidepanel(sprites, &clickSfx, dispatchAction);
   initConfirmOverlay(sprites, &clickSfx);
   initProgressOverlay(sprites, &clickSfx);
   initAudioPlayerOverlay(sprites);
   initVideoPlayerOverlay(sprites);
   initTextEditorOverlay(sprites);
   initHexViewerOverlay(sprites);
   initKeyboard(sprites, spriteRegions[SPRITE_HIGHLIGHT], 7);   // 7 = highlight sprite's 9-slice corner cap
   initHexPad(sprites, spriteRegions[SPRITE_HIGHLIGHT], 7);
   addFooterButton(PAD_BTN_TRIANGLE, GLYPH_TRIANGLE, "Options", openSidepanel);
   addFooterButton(PAD_BTN_START, GLYPH_START, "Search", NULL);   // hint only - search isn't implemented yet
   setFooterButtonEnabled(PAD_BTN_START, 0);
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

static inline int anyOverlayVisible(void)
{
   return isOverlayVisible(&sidepanel)
      || isOverlayVisible(&confirmOverlay)
      || isOverlayVisible(&progressOverlay)
      || isOverlayVisible(&imageViewerOverlay)
      || isOverlayVisible(&audioPlayerOverlay)
      || isOverlayVisible(&videoPlayerOverlay)
      || isOverlayVisible(&textEditorOverlay)
      || isOverlayVisible(&hexViewerOverlay);
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
   updateKeyboard();
   updateHexPad();

   if (!overlayWasVisible) {
      updateFooterWidget();
      updateClockWidget();
      setFreeSpacePath(getCurrentPath());   // report the volume the user is in
      updateFreeSpaceWidget();
      updateFileList();
   }
}

static void drawHome(void)
{
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), COLOR_APP_BG);
   drawNineSlice(&breadcrumbBox);
   drawImage(&titleFolderIcon);
   drawImage(&clockIcon);
   drawLabel(&titleLabel);
   fillGfxRectangle(DIVIDER_X, DIVIDER_TOP_Y, DIVIDER_W, 2, COLOR_DIVIDER);
   fillGfxRectangle(DIVIDER_X, DIVIDER_BOT_Y, DIVIDER_W, 2, COLOR_DIVIDER);
   drawBreadcrumb(&breadcrumb);
   drawClockWidget();
   drawFreeSpaceWidget();
   drawFileList();
   drawFooterWidget();
   drawOverlay(&imageViewerOverlay);
   drawOverlay(&audioPlayerOverlay);
   drawOverlay(&videoPlayerOverlay);
   drawOverlay(&textEditorOverlay);
   drawOverlay(&hexViewerOverlay);
   drawKeyboard();
   drawHexPad();
   drawOverlay(&sidepanel);
   drawOverlay(&confirmOverlay);
   drawOverlay(&progressOverlay);
}

static void suspendHome(void) {}

static void termHome(void)
{
   stopFtpServer();
   termKeyboard();
   termHexPad();
   termOverlay(&hexViewerOverlay);
   termOverlay(&textEditorOverlay);
   termOverlay(&videoPlayerOverlay);
   termOverlay(&audioPlayerOverlay);
   termOverlay(&imageViewerOverlay);
   termOverlay(&progressOverlay);
   termOverlay(&confirmOverlay);
   termOverlay(&sidepanel);
   termFileList();
   termFooterWidget();
   termClockWidget();
   termFreeSpaceWidget();
   termBreadcrumb(&breadcrumb);
   freeLabel(&titleLabel);

   // release the screen's own textures (the RSX is already idle after the
   // widget teardown above, but make sure before reclaiming their VRAM).
   finishGfx();
   freeGfxTexture(&sprites);

   freeAudio(&clickSfx);
   freeAudio(&checkSfx);
   closeFont(&pop);
}

Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
