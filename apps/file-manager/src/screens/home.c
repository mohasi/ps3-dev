#include "screens/home.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "ui/image.h"
#include "ui/breadcrumb.h"
#include "widgets/clock-widget.h"
#include "widgets/free-space-widget.h"

static Font pop;
static GfxTexture bg;
static GfxTexture spritesheet;
static Image background;
static Breadcrumb breadcrumb;

static void homeInit(void)
{
    // resources
    pop = openSystemFont(FONT_POP);
    bg = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/background.png");
    spritesheet = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/spritesheet.png");

    // background
    initImage(&background, bg, 0, 0, AUTO, AUTO, AUTO, AUTO, AUTO, AUTO, GFX_FILTER_LINEAR);

    // breadcrumb
    initBreadcrumb(&breadcrumb, &pop, 75, 130, COLOR_WHITE, spritesheet, 0, 0, 7, 12, 20);
    pushBreadcrumb(&breadcrumb, "dev_hdd0");
    pushBreadcrumb(&breadcrumb, "GAMES");
    pushBreadcrumb(&breadcrumb, "My Game");

    // widgets
    initClockWidget(&pop, 1675, 55, 21, COLOR_WHITE);
    initFreeSpaceWidget(&pop, 1794, 953, 20, 0x64FFFFFF, 80);
}

static void homeResume(void) {}

static void homeUpdate(void)
{
    updateClockWidget();
    updateFreeSpaceWidget();
}

static void homeDraw(void)
{
    drawImage(&background);
    drawBreadcrumb(&breadcrumb);
    drawClockWidget();
    drawFreeSpaceWidget();
}

static void homeSuspend(void) {}

static void homeTerm(void)
{
    termBreadcrumb(&breadcrumb);
    closeFont(&pop);
}

Screen homeScreen = { homeInit, homeResume, homeUpdate, homeDraw, homeSuspend, homeTerm, SCREEN_TERMINATED };
