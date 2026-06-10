// clock-widget - date/time label refreshed every second
#include "widgets/clock-widget.h"
#include "ui/label.h"
#include "timer.h"
#include <cell/rtc.h>

static Label label;
static Timer timer;

static void refresh(void *ctx)
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

    setLabelText(&label, buf);
}

void initClockWidget(Font *font, int x, int y, int size, uint32_t color)
{
    initLabel(&label, font, x, y, AUTO, AUTO, size, color, TEXT_NOWRAP, NULL);
    initTimer(&timer, 1000000, refresh, NULL);
    refresh(NULL);
}

void updateClockWidget(void)
{
    updateTimer(&timer);
}

void drawClockWidget(void)
{
    drawLabel(&label);
}

void termClockWidget(void)
{
    freeLabel(&label);
}
