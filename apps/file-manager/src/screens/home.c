#include "screens/home.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "ui/image.h"
#include "ui/breadcrumb.h"
#include "widgets/clock-widget.h"
#include "widgets/free-space-widget.h"
#include "widgets/file-list.h"
#include "sprites.h"
#include <string.h>

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
    initImage(&background, bg, 0, 0, AUTO, AUTO, SPRITE_FULL, GFX_FILTER_LINEAR);

    // breadcrumb
    initBreadcrumb(&breadcrumb, &pop, 75, 130, COLOR_WHITE, spritesheet, sprites[SPRITE_CHEVRON], 20);

    // widgets
    initClockWidget(&pop, 1675, 55, 21, COLOR_WHITE);
    initFreeSpaceWidget(&pop, 1794, 953, 20, 0x64FFFFFF, 80);

    // file list
    initFileList(&pop, spritesheet, 183, 244, 1200, 74, 24, COLOR_WHITE, &breadcrumb);
}

static void homeResume(void) {}

static void homeUpdate(void)
{
    updateClockWidget();
    updateFreeSpaceWidget();
    updateFileList();
}

static void homeDraw(void)
{
    drawImage(&background);
    drawBreadcrumb(&breadcrumb);
    drawClockWidget();
    drawFreeSpaceWidget();
    drawFileList();
}

static void homeSuspend(void) {}

static void homeTerm(void)
{
    termFileList();
    termBreadcrumb(&breadcrumb);
    closeFont(&pop);
}

Screen homeScreen = { homeInit, homeResume, homeUpdate, homeDraw, homeSuspend, homeTerm, SCREEN_TERMINATED };
