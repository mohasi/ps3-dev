/* simple-ftp — minimal FTP server for PS3 VSH.
 * Anonymous, binary-only, PASV-mode only. Listens on :21.
 * See ftp.h for the server itself; this file is just the plugin entry. */

#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#include "dbg.h"
#include "vsh.h"
#include "syscall.h"
#include "ftp.h"

SYS_MODULE_INFO(SimpleFtp, 0, 1, 1);
SYS_MODULE_START(_start);

static void pluginThread(uint64_t arg)
{
    (void)arg;
    logInfo("[ftp] plugin thread start\n");

    /* Mount /dev_blind so FTP clients can write to /dev_flash via that
     * mount point. webMAN-MOD exposes this as an "Enable /dev_blind on
     * startup" option; we enable it unconditionally because the whole
     * point of this plugin is unrestricted filesystem access. The mount
     * syscall is idempotent-ish — a second call just returns an error,
     * which we log for visibility but otherwise ignore. */
    int64_t mrc = mountDevBlind();
    logError("[ftp] mount /dev_blind rc 0x%x\n", (int)mrc);

    /* Wait for XMB readiness so the network stack is up. 60s budget. */
    int ticks = 0;
    while (!isXmbReady()) {
        sys_timer_sleep(1);
        if (++ticks > 60) {
            logError("[ftp] xmb ready timeout\n");
            sys_ppu_thread_exit(0);
            return;
        }
    }
    logInfo("[ftp] xmb ready\n");

    /* Hand control to the FTP listener thread. It owns the port 21 socket
     * and spawns a session thread per accepted client. */
    sys_ppu_thread_t tid;
    sys_ppu_thread_create(&tid, ftpListenerThread, 0, 0x400, 0x1800,
                          SYS_PPU_THREAD_CREATE_JOINABLE, "ftpd");

    sys_ppu_thread_exit(0);
}

int _start(uint64_t arg)
{
    (void)arg;
    logInfo("[ftp] _start\n");

    sys_ppu_thread_t tid;
    sys_ppu_thread_create(&tid, pluginThread, 0, 0x400, 0x4000,
                          SYS_PPU_THREAD_CREATE_JOINABLE, "sftp");
    return SYS_PRX_RESIDENT;
}
