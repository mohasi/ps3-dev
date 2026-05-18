// timer - polling interval timer with callback
#include "timer.h"
#include <sys/sys_time.h>
#include <cell/sysmodule.h>

static int rtcLoaded = 0;

void initTimer(Timer *t, uint64_t intervalUs, TimerCallback callback, void *ctx)
{
    if (!rtcLoaded) {
        cellSysmoduleLoadModule(CELL_SYSMODULE_RTC);
        rtcLoaded = 1;
    }
    t->intervalUs = intervalUs;
    t->callback = callback;
    t->ctx = ctx;
    t->lastFireUs = sys_time_get_system_time();
}

void updateTimer(Timer *t)
{
    uint64_t now = sys_time_get_system_time();
    if (now - t->lastFireUs >= t->intervalUs) {
        t->lastFireUs += t->intervalUs;
        // catch up if we fell behind without firing multiple times
        if (now - t->lastFireUs >= t->intervalUs)
            t->lastFireUs = now;
        if (t->callback)
            t->callback(t->ctx);
    }
}
