// simple-ftp — minimal FTP server for PS3 VSH.
// Anonymous, binary-only, PASV-mode only. Shared core FTP server on :21;
// this file only handles plugin boot concerns.

#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "dbg.h"
#include "vsh.h"
#include "syscall.h"
#include "ftp.h"
#include "vfs.h"
#include "thread.h"
#include "bridge-client.h"

SYS_MODULE_INFO(SimpleFtp, 0, 1, 1);
SYS_MODULE_START(_start);

static void pluginThread(uint64_t arg)
{
   (void)arg;
   logInfo("[ftp] plugin thread start\n");

   // wait for XMB readiness so the network stack is up. 60s budget.
   int ticks = 0;
   while (!isXmbReady()) {
      sys_timer_sleep(1);
      if (++ticks > 60) {
         logError("[ftp] xmb ready timeout\n");
         exitThread();
         return;
      }
   }
   logInfo("[ftp] xmb ready\n");

   // startFtpServer retries socket creation while the network stack comes up.
   FtpResult result = startFtpServer(FTP_DEFAULT_PORT);
   if (result != FTP_OK) {
      logError("[ftp] failed to start ftp server on :%d (rc %d)\n", FTP_DEFAULT_PORT, (int)result);
      exitThread();
      return;
   }

   // Bring up the VFS (exFAT backend + path routing) and poll for hotplug here, on this plugin
   // thread - deliberately OFF the FTP listener/session path so a slow or contended USB storage
   // probe (e.g. a stick the file-manager also has open) can never stall accepting or serving
   // connections. FTP serves the HDD regardless; exFAT volumes surface here once mounted.
   initVfs();
   while (1) {
      pollMounts();
      sys_timer_sleep(1);
   }
}

int _start(uint64_t arg)
{
   (void)arg;
   registerWithBridge("plugin", "ftp");
   logInfo("[ftp] _start\n");

   sys_ppu_thread_t tid;
   spawnThread(&tid, pluginThread, 0, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "ftp-main");
   return SYS_PRX_RESIDENT;
}
