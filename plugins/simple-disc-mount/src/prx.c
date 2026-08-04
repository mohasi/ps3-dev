// Simple Disc Mount — VSH plugin.
//
// Adds "Mount Disc Image" below "Package Manager" in the XMB Games column,
// populating a submenu with every .iso in /dev_hdd0/PS3ISO. Items wake
// Sony's webrender_plugin with http://0:8947/mount/<name>, which our
// in-process HTTP listener catches and turns into a Cobra PS3 disc mount.
//
// Boot order on the plugin thread:
//   1. wait for XMB ready
//   2. auto-mount last ISO if the drive is empty and sdm_last.txt remembers one
//   3. spawn the HTTP listener (X-press on items) and the disc watcher (a real
//      disc takes over from a mounted image) — before the XML work, so a menu
//      problem cannot leave the plugin with nothing serving it
//   4. open /dev_blind, ensure xmlhost directories exist
//   5. regenerate sdm.xml (ISO list) and patch category_game.xml once

#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "dbg.h"
#include "vsh.h"
#include "syscall.h"
#include "vfs.h"
#include "thread.h"
#include "disc-mount.h"
#include "xmb-inject.h"
#include "http.h"
#include "bridge-client.h"

SYS_MODULE_INFO(SimpleDiscMount, 0, 1, 1);
SYS_MODULE_START(_start);

static void autoMountLast(void)
{
   char path[SDM_PATH_MAX];

   // a real disc always wins — never mount an image over it
   if (getRealDiscType() != 0) {
      logInfo("[sdm] real disc present, skipping auto-mount\n");
      forgetLastMountedImage();
      return;
   }

   if (getLastMountedImage(path, sizeof path) == 0) return;

   // check iso still exists
   if (!fileExists(path)) {
      logError("[sdm] auto-mount target missing: %s\n", path);
      return;
   }

   // mount it
   if (mountDiscImage(path) == 0) {
      logInfo("[sdm] auto-mounted: %s\n", path);
   } else {
      logError("[sdm] auto-mount failed: %s\n", path);
   }
}

enum {
   DISC_POLL_SECONDS  = 2,
   DISC_RETRY_SECONDS = 30,   // after a failed unmount, so we don't replay events in a tight loop
};

// A physically inserted disc is the user's intent, so it overrides a mounted
// image. Cobra keeps reporting the emulated disc to the XMB until something
// unmounts it, and nothing tells us an insert happened, so poll the real disc
// type. Waiting for a game to exit avoids pulling the disc out from under a
// game that is running off the image.
static void discWatchThread(uint64_t arg)
{
   (void)arg;
   logInfo("[sdm] disc watch thread start\n");

   unsigned int pollSeconds = DISC_POLL_SECONDS;
   for (;;) {
      sys_timer_sleep(pollSeconds);

      if (getRealDiscType() == 0 || getGameProcessId() != 0) continue;

      switch (unmountDiscImage()) {
      case UNMOUNT_DONE:
         logInfo("[sdm] real disc inserted, disc image unmounted\n");
         vshNotify("Real disc inserted, disc image unmounted.");
         pollSeconds = DISC_POLL_SECONDS;
         break;

      case UNMOUNT_NOTHING_MOUNTED:
         break;   // the common case: a real disc in the drive and no image to displace

      case UNMOUNT_FAILED:
         logError("[sdm] unmount failed, retrying in %ds\n", DISC_RETRY_SECONDS);
         pollSeconds = DISC_RETRY_SECONDS;
         break;
      }
   }
}

static void pluginThread(uint64_t arg)
{
   (void)arg;
   logInfo("[sdm] plugin thread start\n");

   // Wait for XMB readiness, with a ~60s budget.
   int ticks = 0;
   while (!isXmbReady()) {
      sys_timer_sleep(1);
      if (++ticks > 60) {
         logError("[sdm] xmb ready timeout\n");
         exitThread();
         return;
      }
   }
   logInfo("[sdm] xmb ready\n");

   // Give the storage/BD subsystem time to finish initialising.
   // isXmbReady() fires before the disc driver is fully up — mounting
   // immediately leads to 80010516 ("game could not be started") because
   // the system hasn't registered the virtual BD device yet. The manual
   // XMB path works because the user navigates for several seconds first.
   sys_timer_sleep(5);

   // Re-mount the last ISO before we touch XML so the fake-disc-insert
   // event races in alongside the XMB's first paint — the BD icon tends to
   // show up the moment the Games column settles.
   autoMountLast();

   // Spawn the workers before the XML work: the listener owns :8947 (loopback)
   // and turns incoming GET /mount/<name> into cobraMountIso calls, and the
   // watcher enforces "a real disc wins". An XML problem must not take either
   // down, or the menu from a previous boot renders with nothing serving it.
   sys_ppu_thread_t httpTid;
   int rc = spawnThread(&httpTid, httpListenerThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "sdm-http");
   if (rc != 0) logError("[sdm] http thread spawn rc=0x%x\n", rc);

   sys_ppu_thread_t watchTid;
   rc = spawnThread(&watchTid, discWatchThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "sdm-watch");
   if (rc != 0) logError("[sdm] disc watch thread spawn rc=0x%x\n", rc);

   // menu generation
   mountDevBlind();
   logInfo("[sdm] dev_blind mounted\n");

   if (makeDir(pathXmlHostRoot) != 0 || makeDir(pathXmlHostGp) != 0) {
      logError("[sdm] mkdir xmlhost failed\n");
      exitThread();
      return;
   }

   if (openXmlScratch() != 0) { exitThread(); return; }

   int patched = (writeSdmXml() == 0) ? patchCategoryGameXml() : PATCH_FAILED;
   closeXmlScratch();

   // Notify only on a fresh install. Sleep past webMAN's own boot toast
   // so ours isn't stomped.
   if (patched == PATCH_APPLIED) {
      sys_timer_sleep(10);
      logInfo("[sdm] vshNotify\n");
      vshNotify("Simple disc mount plugin installed successfully!");
   }

   logInfo("[sdm] done\n");
   exitThread();
}

int _start(uint64_t arg)
{
   (void)arg;
   registerWithBridge("plugin", "sdm");
   logInfo("[sdm] _start\n");
   logBuildVersion();

   sys_ppu_thread_t tid;
   int rc = spawnThread(&tid, pluginThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "sdm-main");
   if (rc != 0) logError("[sdm] plugin thread spawn rc=0x%x\n", rc);
   return SYS_PRX_RESIDENT;
}
