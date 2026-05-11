#include "screens/home.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "ui/image.h"
#include "ui/label.h"
#include "ui/breadcrumb.h"
#include "timer.h"
#include <cell/rtc.h>

static Font pop;
static GfxTexture bg;
static GfxTexture spritesheet;
static Image background;
static Breadcrumb breadcrumb;
static Label timeLabel;
static Timer clockTimer;

static void refreshClock(void *ctx)
{
    (void)ctx;
    CellRtcDateTime dt;
    cellRtcGetCurrentClockLocalTime(&dt);

    char buf[16];
    int p = 0;
    buf[p++] = '0' + (dt.day / 10);
    buf[p++] = '0' + (dt.day % 10);
    buf[p++] = '/';
    buf[p++] = '0' + (dt.month / 10);
    buf[p++] = '0' + (dt.month % 10);
    buf[p++] = ' ';
    buf[p++] = ' ';
    buf[p++] = ' ';
    buf[p++] = '0' + (dt.hour / 10);
    buf[p++] = '0' + (dt.hour % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (dt.minute / 10);
    buf[p++] = '0' + (dt.minute % 10);
    buf[p] = '\0';

    setLabelText(&timeLabel, buf);
}

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

    // clock
    initLabel(&timeLabel, &pop, 1675, 55, AUTO, AUTO, 21, COLOR_WHITE, TEXT_NOWRAP, NULL);
    initTimer(&clockTimer, 1000000, refreshClock, NULL);
    refreshClock(NULL);
}

static void homeResume(void) {}

static void homeUpdate(void)
{
    updateTimer(&clockTimer);
}

static void homeDraw(void)
{
    // background
    drawImage(&background);

    // breadcrumb
    drawBreadcrumb(&breadcrumb);

    // clock
    drawLabel(&timeLabel);
}

static void homeSuspend(void) {}

static void homeTerm(void)
{
    termBreadcrumb(&breadcrumb);
    closeFont(&pop);
}

Screen homeScreen = { homeInit, homeResume, homeUpdate, homeDraw, homeSuspend, homeTerm, SCREEN_TERMINATED };
