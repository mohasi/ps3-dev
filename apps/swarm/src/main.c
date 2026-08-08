#include <sys/process.h>

#include "app.h"
#include "bridge-client.h"
#include "dbg.h"
#include "font.h"
#include "gfx.h"
#include "log-store.h"
#include "pad.h"
#include "screen-manager.h"
#include "screens/home.h"
#include "settings.h"
#include "string-utilities.h"
#include "torrent-selftest.h"
#include "torrent-service.h"
#include "widgets/theme.h"
#include "vfs.h"
#include "wg-config.h"
#include "wg-selftest.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

// What the config file holds, for a log that was asked for. Never any key material, and never by
// default: the addresses in it say which VPN account this console uses.
static void logConfigSummary(const char *configPath)
{
   WgConfig config;
   if (loadWgConfig(&config, configPath) != 0) return;

   char tunnelAddress[16], dnsAddress[16];
   formatIpv4(tunnelAddress, sizeof tunnelAddress, config.tunnelAddress);
   formatIpv4(dnsAddress, sizeof dnsAddress, config.dnsAddress);

   logTrace("[swarm] config: endpoint %s:%d, tunnel ip %s/%d, dns %s, keepalive %ds, all traffic %d\n",
            config.endpointHost, config.endpointPort, tunnelAddress, config.tunnelPrefixLength, dnsAddress,
            config.keepaliveSeconds, config.routesAllTraffic);
}

// the app itself: one screen, with the tunnel and the transfers on their own thread behind it
static int runApp(const char *configPath)
{
   // vsync on so the drawing thread waits for the display each frame. without it this loop never
   // yields, and the network thread behind it gets almost no time.
   if (initGfx(GFX_VSYNC_ON) != 0) return 1;
   if (initFont() != 0) return 1;
   initPad();
   startLogStore();   // the Logs view reads these; the bridge still gets its copy

   startTorrentService(configPath);
   changeScreen(&homeScreen);

   while (!appExitRequested) {
      appPoll();
      updatePad();
      updateScreen();

      beginGfxFrame();
      clearGfx(BACKDROP);
      drawScreen();
      endGfxFrame();
   }

   changeScreen(NULL);
   stopTorrentService();
   stopLogStore();
   termFont();
   termGfx();
   return 0;
}

// Swarm downloads torrents through a WireGuard tunnel that it speaks itself.

int main(int argc, char **argv)
{
   (void)argc;
   (void)argv;

   initRtc();
   initVfs();
   appRegisterExitCallback();

   initNet();
   registerWithBridge("app", "swarm");   // live logs in the bridge client's Logs tab
   logBuildVersion();

   loadSwarmSettings();
   setLogDetailed(isDetailedLogWanted());   // off unless settings.txt says logs=full

   int failures = runWgSelfTest() + runTorrentSelfTest();

   logConfigSummary(getWgConfigPath());

   runApp(getWgConfigPath());

   shutdownVfs();
   return failures == 0 ? 0 : 1;
}
