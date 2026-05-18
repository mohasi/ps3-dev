/* Simple Disc Mount — VSH plugin.
 *
 * Adds "Mount Disc Image" below "Package Manager" in the XMB Games column,
 * populating a submenu with every .iso in /dev_hdd0/PS3ISO. Items wake
 * Sony's webrender_plugin with http://0:8947/mount/<name>, which our
 * in-process HTTP listener catches and turns into a Cobra PS3 disc mount.
 *
 * Boot order on the plugin thread:
 *   1. wait for XMB ready
 *   2. auto-mount last ISO if sdm_last.txt remembers one
 *   3. open /dev_blind, ensure xmlhost directories exist
 *   4. regenerate sdm.xml (ISO list) and patch category_game.xml once
 *   5. spawn the HTTP listener so X-press on items actually mounts */

#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "dbg.h"
#include "vsh.h"
#include "syscall.h"
#include "file.h"
#include "cobra.h"
#include "xmb-inject.h"
#include "http.h"
#include "bridge-client.h"

SYS_MODULE_INFO(SimpleDiscMount, 0, 1, 1);
SYS_MODULE_START(_start);

static void autoMountLast(void)
{
    char path[SDM_PATH_MAX];

    // read last mount path from file
    if (readFile(pathLastMount, path, sizeof path) < 0) return;

    // check iso still exists
    if (!fileExists(path)) {
        logError("[sdm] auto-mount target missing: %s\n", path);
        return;
    }

    // mount it
    if (cobraMountIso(path) == 0) {
        logInfo("[sdm] auto-mounted: %s\n", path);
    } else {
        logError("[sdm] auto-mount failed: %s\n", path);
    }
}

static void pluginThread(uint64_t arg)
{
    (void)arg;
    logInfo("[sdm] plugin thread start\n");

    /* Wait for XMB readiness, with a ~60s budget. */
    int ticks = 0;
    while (!isXmbReady()) {
        sys_timer_sleep(1);
        if (++ticks > 60) {
            logError("[sdm] xmb ready timeout\n");
            sys_ppu_thread_exit(0);
            return;
        }
    }
    logInfo("[sdm] xmb ready\n");

    /* Give the storage/BD subsystem time to finish initialising.
     * isXmbReady() fires before the disc driver is fully up — mounting
     * immediately leads to 80010516 ("game could not be started") because
     * the system hasn't registered the virtual BD device yet. The manual
     * XMB path works because the user navigates for several seconds first. */
    sys_timer_sleep(5);

    /* Re-mount the last ISO before we touch XML so the fake-disc-insert
     * event races in alongside the XMB's first paint — the BD icon tends to
     * show up the moment the Games column settles. */
    autoMountLast();

    mountDevBlind();
    logInfo("[sdm] dev_blind mounted\n");

    if (makeDir(pathXmlHostRoot) != 0 || makeDir(pathXmlHostGp) != 0) {
        logError("[sdm] mkdir xmlhost failed\n");
        sys_ppu_thread_exit(0);
        return;
    }

    if (writeSdmXml() != 0) { sys_ppu_thread_exit(0); return; }

    int rc = patchCategoryGameXml();

    /* Notify only on a fresh install. Sleep past webMAN's own boot toast
     * so ours isn't stomped. */
    if (rc == PATCH_APPLIED) {
        sys_timer_sleep(10);
        logInfo("[sdm] vshNotify\n");
        vshNotify("Simple disc mount plugin installed successfully!");
    }

    /* Hand off to the HTTP listener. It owns :8947 (loopback) and turns
     * incoming GET /mount/<name> into cobraMountIso calls. */
    sys_ppu_thread_t hid;
    sys_ppu_thread_create(&hid, httpListenerThread, 0, 0x400, 0x2000, SYS_PPU_THREAD_CREATE_JOINABLE, "sdmh");

    logInfo("[sdm] done\n");
    sys_ppu_thread_exit(0);
}

int _start(uint64_t arg)
{
    (void)arg;
    registerWithBridge("plugin", "simple-disc-mount");
    logInfo("[sdm] _start\n");

    sys_ppu_thread_t tid;
    sys_ppu_thread_create(&tid, pluginThread, 0, 0x400, 0x4000, SYS_PPU_THREAD_CREATE_JOINABLE, "sdm");
    return SYS_PRX_RESIDENT;
}
