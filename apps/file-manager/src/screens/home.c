#include "screens/home.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "ui/image.h"
#include "ui/label.h"
#include "ui/breadcrumb.h"

static Font pop;
static GfxTexture bg;
static GfxTexture icons;
static Label title;
static Image background;
static Image logo;
static Breadcrumb breadcrumb;

static void homeInit(void)
{
    // resources
    pop = fontOpenSystem(FONT_POP);
    bg = gfxLoadTexture("/dev_hdd0/game/FILEMGR01/USRDIR/background.png");
    icons = gfxLoadTexture("/dev_hdd0/game/FILEMGR01/USRDIR/icons.png");

    // background
    imageInit(&background, bg, 0, 0, 1920, 1080, AUTO, AUTO, AUTO, AUTO, GFX_FILTER_LINEAR);

    // logo & title
    imageInit(&logo, icons, 42, 20, AUTO, AUTO, 10, 132, 68, 57, GFX_FILTER_NEAREST);
    labelInit(&title, &pop, 140, 5, AUTO, AUTO, 30, COLOR_WHITE, TEXT_NOWRAP);
    labelSetText(&title, "PS3 File Manager");

    // breadcrumb
    breadcrumbInit(&breadcrumb, &pop, 40, 100, 1840, 43, 0xFF010B1C, 0xFF161C2C, COLOR_WHITE, 0xFF404653, 5, 2, 18);
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

    // logo & title
    imageDraw(&logo);
    labelDraw(&title);

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
