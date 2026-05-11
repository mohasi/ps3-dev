#include "screens/home.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "ui/image.h"
#include "ui/breadcrumb.h"

static Font pop;
static GfxTexture bg;
static GfxTexture spritesheet;
static Image background;
static Breadcrumb breadcrumb;

static void homeInit(void)
{
    // resources
    pop = fontOpenSystem(FONT_POP);
    bg = gfxLoadTexture("/dev_hdd0/game/FILEMGR01/USRDIR/background.png");
    spritesheet = gfxLoadTexture("/dev_hdd0/game/FILEMGR01/USRDIR/spritesheet.png");

    // background
    imageInit(&background, bg, 0, 0, AUTO, AUTO, AUTO, AUTO, AUTO, AUTO, GFX_FILTER_LINEAR);

    // breadcrumb
    breadcrumbInit(&breadcrumb, &pop, 75, 130, COLOR_WHITE, spritesheet, 0, 0, 7, 12, 20);
    breadcrumbPush(&breadcrumb, "dev_hdd0");
    breadcrumbPush(&breadcrumb, "GAMES");
    breadcrumbPush(&breadcrumb, "My Game");
}

static void homeResume(void) {}

static void homeUpdate(void) {}

static void homeDraw(void)
{
    // background
    imageDraw(&background);

    // breadcrumb
    breadcrumbDraw(&breadcrumb);
}

static void homeSuspend(void) {}

static void homeTerm(void)
{
    breadcrumbTerm(&breadcrumb);
    fontClose(&pop);
}

Screen homeScreen = { homeInit, homeResume, homeUpdate, homeDraw, homeSuspend, homeTerm, SCREEN_TERMINATED };
