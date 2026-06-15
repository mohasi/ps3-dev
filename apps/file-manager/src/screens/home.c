#include "screens/home.h"
#include "gfx.h"
#include "audio.h"
#include "colors.h"
#include "font.h"
#include "ftp.h"
#include "ui/image.h"
#include "ui/breadcrumb.h"
#include "widgets/clock-widget.h"
#include "widgets/footer-widget.h"
#include "widgets/free-space-widget.h"
#include "widgets/file-list.h"
#include "overlays/sidepanel.h"
#include "overlays/confirm-overlay.h"
#include "overlays/progress-overlay.h"
#include "overlays/image-viewer-overlay.h"
#include "overlays/audio-player-overlay.h"
#include "file-actions.h"
#include "sprite-regions.h"
#include "string-utilities.h"
#include "network.h"

static Font pop;
static GfxTexture bg;
static GfxTexture sprites;
static Image background;
static Breadcrumb breadcrumb;
static Audio clickSfx;
static Audio checkSfx;

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
    bg       = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/background.png");
    sprites  = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/sprites.png");
    clickSfx = loadSfx("/dev_hdd0/game/FILEMGR01/USRDIR/click.wav", SFX_MEMORY);
    checkSfx = loadSfx("/dev_hdd0/game/FILEMGR01/USRDIR/check.wav", SFX_MEMORY);

    initImage(&background, bg, 0, 0, AUTO, AUTO, SPRITE_FULL, GFX_FILTER_LINEAR);
    initBreadcrumb(&breadcrumb, &pop, 70, 130, COLOR_WHITE, sprites, spriteRegions[SPRITE_CHEVRON], 20);
    initClockWidget(&pop, 1675, 55, 21, COLOR_WHITE);
    initFreeSpaceWidget(&pop, 1794, 953, 20, 0x64FFFFFF, 80);
    initFooterWidget(&pop, sprites);
    initFileList(&pop, sprites, &clickSfx, &checkSfx, 177, 244, 1200, 74, 24, COLOR_WHITE, &breadcrumb);
    initSidepanel(sprites, &clickSfx, dispatchAction);
    initConfirmOverlay(sprites, &clickSfx);
    initProgressOverlay(sprites, &clickSfx);
    initAudioPlayerOverlay(sprites);
    addFooterButton(PAD_BTN_TRIANGLE, spriteRegions[SPRITE_TRIANGLE], "Options", openSidepanel);
    addFooterButton(PAD_BTN_SELECT, spriteRegions[SPRITE_SELECT], "Start FTP Server", toggleFtpServer);

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
        || isOverlayVisible(&audioPlayerOverlay);
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

    if (!overlayWasVisible) {
        updateFooterWidget();
        updateClockWidget();
        updateFreeSpaceWidget();
        updateFileList();
    }
}

static void drawHome(void)
{
    drawImage(&background);
    drawBreadcrumb(&breadcrumb);
    drawClockWidget();
    drawFreeSpaceWidget();
    drawFileList();
    drawFooterWidget();
    drawOverlay(&imageViewerOverlay);
    drawOverlay(&audioPlayerOverlay);
    drawOverlay(&sidepanel);
    drawOverlay(&confirmOverlay);
    drawOverlay(&progressOverlay);
}

static void suspendHome(void) {}

static void termHome(void)
{
    stopFtpServer();
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

    // release the screen's own textures (the RSX is already idle after the
    // widget teardown above, but make sure before reclaiming their VRAM).
    finishGfx();
    freeGfxTexture(&bg);
    freeGfxTexture(&sprites);

    freeSfx(&clickSfx);
    freeSfx(&checkSfx);
    closeFont(&pop);
}

Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
