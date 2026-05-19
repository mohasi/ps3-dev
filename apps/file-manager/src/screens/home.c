#include "screens/home.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "ui/image.h"
#include "ui/breadcrumb.h"
#include "widgets/clock-widget.h"
#include "widgets/free-space-widget.h"
#include "widgets/file-list.h"
#include "sprite-regions.h"
#include <string.h>

static Font pop;
static GfxTexture bg;
static GfxTexture sprites;
static Image background;
static Breadcrumb breadcrumb;

static void initHome(void)
{
    // resources
    pop = openSystemFont(FONT_POP);
    bg = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/background.png");
    sprites = loadGfxTexture("/dev_hdd0/game/FILEMGR01/USRDIR/sprites.png");

    // background
    initImage(&background, bg, 0, 0, AUTO, AUTO, SPRITE_FULL, GFX_FILTER_LINEAR);

    // breadcrumb
    initBreadcrumb(&breadcrumb, &pop, 75, 130, COLOR_WHITE, sprites, spriteRegions[SPRITE_CHEVRON], 20);

    // widgets
    initClockWidget(&pop, 1675, 55, 21, COLOR_WHITE);
    initFreeSpaceWidget(&pop, 1794, 953, 20, 0x64FFFFFF, 80);

    // file list
    initFileList(&pop, sprites, 177, 244, 1200, 74, 24, COLOR_WHITE, &breadcrumb);
}

static void resumeHome(void) {}

static void updateHome(void)
{
    updateClockWidget();
    updateFreeSpaceWidget();
    updateFileList();
}

static void drawHome(void)
{
    drawImage(&background);
    drawBreadcrumb(&breadcrumb);
    drawClockWidget();
    drawFreeSpaceWidget();
    drawFileList();
}

static void suspendHome(void) {}

static void termHome(void)
{
    termFileList();
    termBreadcrumb(&breadcrumb);
    closeFont(&pop);
}

Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
