/* Simple Disc Mount — VSH plugin.
 * Adds "Mount Disc Image" below "Package Manager" in the XMB Games column
 * and populates a submenu listing every .iso in /dev_hdd0/PS3ISO. Items
 * currently use Sony's stock seg_dummy_items pattern (explore_plugin +
 * NotifyErrorNoExecute) as a placeholder — X-press shows "Cannot operate"
 * instead of doing anything. Real mount wiring will come via a
 * webrender_plugin Action() hook. */

#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "dbg.h"
#include "vsh.h"
#include "xmb-inject.h"

SYS_MODULE_INFO(SimpleDiscMount, 0, 1, 1);
SYS_MODULE_START(_start);

static void pluginThread(uint64_t arg)
{
    (void)arg;
    dbgLog("[sdm] plugin thread start\n");

    /* Wait for XMB readiness, with a ~60s budget. */
    int ticks = 0;
    while (!isXmbReady()) {
        sys_timer_sleep(1);
        if (++ticks > 60) {
            dbgLog("[sdm] xmb ready timeout\n");
            sys_ppu_thread_exit(0);
            return;
        }
    }
    dbgLog("[sdm] xmb ready\n");

    mountDevBlind();
    dbgLog("[sdm] dev_blind mounted\n");

    if (makeDir(pathXmlHostRoot) != 0 || makeDir(pathXmlHostGp) != 0) {
        dbgLog("[sdm] mkdir xmlhost failed\n");
        sys_ppu_thread_exit(0);
        return;
    }

    if (writeSdmXml() != 0) { sys_ppu_thread_exit(0); return; }

    int rc = patchCategoryGameXml();

    /* Notify only on a fresh install. Sleep past webMAN's own boot toast
     * so ours isn't stomped. */
    if (rc == PATCH_APPLIED) {
        sys_timer_sleep(10);
        dbgLog("[sdm] vshNotify\n");
        vshNotify("Simple Disc Mount: menu installed.");
    }

    dbgLog("[sdm] done\n");
    sys_ppu_thread_exit(0);
}

int _start(uint64_t arg)
{
    (void)arg;
    dbgLog("[sdm] _start\n");

    sys_ppu_thread_t tid;
    sys_ppu_thread_create(&tid, pluginThread, 0, 0x400, 0x4000,
                          SYS_PPU_THREAD_CREATE_JOINABLE, "sdm");
    return SYS_PRX_RESIDENT;
}
