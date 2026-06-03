#include "screens/home.h"
#include "gfx.h"
#include "audio.h"
#include "colors.h"
#include "font.h"
#include "pad.h"
#include "ui/image.h"
#include "ui/breadcrumb.h"
#include "widgets/clock-widget.h"
#include "widgets/free-space-widget.h"
#include "widgets/file-list.h"
#include "overlays/sidepanel.h"
#include "overlays/confirm-overlay.h"
#include "overlays/progress-overlay.h"
#include "file-actions.h"
#include "sprite-regions.h"
#include <string.h>

static Font pop;
static GfxTexture bg;
static GfxTexture sprites;
static Image background;
static Breadcrumb breadcrumb;
static Audio clickSfx;
static Audio checkSfx;

static void initHome(void)
{
    pop      = openSystemFont(FONT_POP);
    bg       = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/background.png");
    sprites  = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/sprites.png");
    clickSfx = loadSfx("/dev_hdd0/game/FILEMGR01/USRDIR/click.wav", SFX_MEMORY);
    checkSfx = loadSfx("/dev_hdd0/game/FILEMGR01/USRDIR/check.wav", SFX_MEMORY);

    initImage(&background, bg, 0, 0, AUTO, AUTO, SPRITE_FULL, GFX_FILTER_LINEAR);
    initBreadcrumb(&breadcrumb, &pop, 75, 130, COLOR_WHITE, sprites, spriteRegions[SPRITE_CHEVRON], 20);
    initClockWidget(&pop, 1675, 55, 21, COLOR_WHITE);
    initFreeSpaceWidget(&pop, 1794, 953, 20, 0x64FFFFFF, 80);
    initFileList(&pop, sprites, &clickSfx, &checkSfx, 177, 244, 1200, 74, 24, COLOR_WHITE, &breadcrumb);
    initSidepanel(sprites, &clickSfx, dispatchAction);
    initConfirmOverlay(sprites, &clickSfx);
}

static void resumeHome(void) {}

static void openSidepanel(void)
{
    int count;
    const SelectionAction *actions = getAvailableActions(&count);
    const SelectionSummary *summary = getSelectionSummary();
    setSidepanelContent(summary, actions, count);
    showOverlay(&sidepanel);
}

static void handleInput(void)
{
    if (pad.btn.triangle == BTN_PRESSED) openSidepanel();
}

static inline int anyOverlayVisible(void)
{
    return sidepanel.status        == OVERLAY_VISIBLE
        || confirmOverlay.status   == OVERLAY_VISIBLE
        || progressOverlay.status  == OVERLAY_VISIBLE;
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

    if (!overlayWasVisible) {
        handleInput();
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
    drawOverlay(&sidepanel);
    drawOverlay(&confirmOverlay);
    drawOverlay(&progressOverlay);
}

static void suspendHome(void) {}

static void termHome(void)
{
    termOverlay(&progressOverlay);
    termOverlay(&confirmOverlay);
    termOverlay(&sidepanel);
    termFileList();
    termBreadcrumb(&breadcrumb);
    freeSfx(&clickSfx);
    freeSfx(&checkSfx);
    closeFont(&pop);
}

Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
