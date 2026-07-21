//
// vfs-init.c - VFS bringup: registers the concrete backends and owns USB hotplug.
//
// Split out from vfs.c on purpose. This is the only translation unit that names
// initExfat/initNtfs, so it is the only one that drags the exFAT/NTFS drivers
// into a link. This toolchain's linker can't strip unreachable code, so a
// consumer that only routes paths (calls openDir/readFs, never initVfs) pulls
// vfs.o alone and stays small; a consumer that brings the VFS up (calls initVfs)
// pulls this object and gets the drivers. The router (vfs.c) names no backend.
//
// The mount registry and its lock live in vfs.c; this file serializes its own
// poll state against the same lock through vfs-internal.h.
//

#include "vfs.h"

#include <stdint.h>
#include <sys/sys_time.h>       // sys_time_get_system_time (pollMounts self-throttle)
#include "storage-device.h"     // isUsbDevicePresent + port count (format-agnostic hotplug)
#include "exfat.h"              // initExfat (brought up as part of the VFS)
#include "ntfs.h"               // initNtfs (brought up as part of the VFS)
#include "thread.h"             // sys_ppu_thread_t + spawn/join/sleep helpers
#include "vfs-internal.h"       // lock primitives + clearMounts, owned by vfs.c

#define VFS_POLL_INTERVAL_US 1000000   // scan hotplug at most once a second, however often callers poll

static int initialized;

// pollMounts debounce + concurrency guard (file-scope so shutdownVfs can reset them). lastScan is
// the last accepted scan time; polling marks a scan in progress so a second caller bows out instead
// of running a duplicate scan (which would double-probe/double-mount a port). Both are read and set
// under mountsLock.
static system_time_t lastScan;   // 0 until the first scan
static int           polling;

// Hotplug poll thread, owned by the VFS so no consumer has to drive polling:
// initVfs spawns it, shutdownVfs stops+joins it. It runs the exFAT mount path,
// but that path keeps its big buffers off the stack (exfat.c bootSector + the
// static sector caches), so an 8 KB stack is enough - which matters on a VSH PRX
// where the thread/stack budget is tight.
static int               pollMounts(void);
static sys_ppu_thread_t  pollThreadTid;
static volatile int      pollThreadStop;
static int               pollThreadRunning;

static void vfsHotplugThread(uint64_t arg)
{
   (void)arg;
   while (!pollThreadStop) {
      pollMounts();        // debounced; fires the mounts-changed callback on change
      sleepMs(1000);       // 1 Hz
   }
   exitThread();
}

// Registered format backends (probe/release/shutdown hooks); set by the app, never by core, so
// libntfs/FatFs link only where they're actually used. The VFS owns USB hotplug detection and
// offers present devices to these backends - they never poll the ports themselves.
#define VFS_MAX_BACKENDS 4
static struct {
   VfsProbeResult (*probe)(int port);
   void           (*release)(int port);
   void           (*shutdown)(void);
} backends[VFS_MAX_BACKENDS];
static int backendCount;

// Per-USB-port hotplug state, owned by the VFS (format-agnostic). present: a device is on the
// port. resolved: we've settled what it is (a backend mounted it, or all declined / cellFs).
// owner: index of the backend that mounted it, or -1.
static uint8_t portPresent[USB_STORAGE_MAX_PORTS];
static uint8_t portResolved[USB_STORAGE_MAX_PORTS];
static int8_t  portOwner[USB_STORAGE_MAX_PORTS];

// invoked by pollMounts when the mount set changes, on whatever thread polled.
static void (*mountsChangedCallback)(void);

void setMountsChangedCallback(void (*callback)(void))
{
   mountsChangedCallback = callback;
}

void registerVfsBackend(VfsProbeResult (*probe)(int port), void (*release)(int port), void (*shutdown)(void))
{
   if (backendCount >= VFS_MAX_BACKENDS) return;
   backends[backendCount].probe    = probe;
   backends[backendCount].release  = release;
   backends[backendCount].shutdown = shutdown;
   backendCount++;
}

// cellFs is the built-in default route; the removable-media backends are brought
// up here so any VFS consumer sees them without a separate call. exFAT is small
// and prx-safe (no libc - just scCall + memCopy), so it can ride into the plugins
// that bring up the VFS. NTFS starts the same way, registered right after exFAT.
void initVfs(void)
{
   if (initialized) return;
   ensureMountsLock();   // create the registry lock before any addVfsMount
   initialized  = 1;
   clearMounts();
   backendCount = 0;
   lastScan     = 0;   // a fresh lifetime must not be debounced against the previous one
   polling      = 0;
   for (int port = 0; port < USB_STORAGE_MAX_PORTS; port++) { portPresent[port] = 0; portResolved[port] = 0; portOwner[port] = -1; }
   initExfat();    // registers the exFAT backend
   initNtfs();     // registers the NTFS backend (probed after exFAT; claims only "NTFS    " volumes)
   pollMounts();   // initial scan so already-inserted volumes appear immediately

   // Own the hotplug cadence: one 8 KB thread, written once, so no consumer drives
   // polling. Self-heals if storage isn't ready yet (it keeps scanning).
   pollThreadStop = 0;
   if (spawnJoinableThread(&pollThreadTid, vfsHotplugThread, 0,
                           THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "vfs-hotplug") == 0)
      pollThreadRunning = 1;
}

// drives each registered backend's hotplug scan, self-throttled to VFS_POLL_INTERVAL_US
// so any caller (the app loop, an ftp listener, ...) can call it as often as it likes
// without putting the sys_storage probe on a hot path. when the mount set changes it
// fires the mounts-changed callback (so a consumer's view refreshes without the caller
// having to inspect the return value), and also returns 1.
static int pollMounts(void)
{
   // accept-or-bow-out, atomically: debounce, and let only one thread scan at a time
   // (the initVfs initial scan races the poll thread's first wake until lastScan is set).
   lockMounts();
   system_time_t now = sys_time_get_system_time();
   if (polling || (lastScan && now - lastScan < VFS_POLL_INTERVAL_US)) { unlockMounts(); return 0; }
   lastScan = now;
   polling  = 1;
   unlockMounts();

   int changed = 0;
   for (int port = 0; port < USB_STORAGE_MAX_PORTS; port++) {
      int present = isUsbDevicePresent(port);   // device-level, format-agnostic (non-DMA, no LED)

      if (present && !portPresent[port]) {                  // device inserted
         portPresent[port]  = 1;
         portResolved[port] = 0;
         portOwner[port]    = -1;
         changed = 1;                                       // refresh now - also surfaces cellFs/FAT32
      } else if (!present && portPresent[port]) {           // device removed
         portPresent[port] = 0;
         if (portOwner[port] >= 0) backends[portOwner[port]].release(port);
         portOwner[port]    = -1;
         portResolved[port] = 0;
         changed = 1;
      }

      // Offer an unresolved present device to each backend until one claims it. NOT_READY means
      // the device can't be read yet (no backend could) - leave it unresolved to retry next poll.
      // If every backend declines, it's a format we don't mount (cellFs handles it, e.g. FAT32).
      if (present && !portResolved[port]) {
         int notReady = 0;
         for (int i = 0; i < backendCount; i++) {
            VfsProbeResult result = backends[i].probe(port);
            if (result == VFS_PROBE_MOUNTED)   { portOwner[port] = (int8_t)i; portResolved[port] = 1; changed = 1; break; }
            if (result == VFS_PROBE_NOT_READY) { notReady = 1; break; }
            // VFS_PROBE_NOT_MINE: try the next backend
         }
         if (!portResolved[port] && !notReady) portResolved[port] = 1;   // none claimed it
      }
   }

   lockMounts();
   polling = 0;
   unlockMounts();

   if (changed && mountsChangedCallback) mountsChangedCallback();
   return changed;
}

void shutdownVfs(void)
{
   pollThreadStop = 1;
   if (pollThreadRunning) { joinThread(pollThreadTid); pollThreadRunning = 0; }

   for (int i = 0; i < backendCount; i++)
      if (backends[i].shutdown) backends[i].shutdown();
   clearMounts();
   backendCount = 0;
   lastScan     = 0;   // don't debounce the next initVfs against this lifetime's last scan
   polling      = 0;
   initialized  = 0;
}
