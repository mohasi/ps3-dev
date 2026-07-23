#include "safety.h"
#include "settings.h"
#include "stress.h"
#include "clocks.h"
#include "dbg.h"

static int cutoffFired;

int hasSafetyCutoffFired(void) { return cutoffFired; }

// the load dials are not the only thing that heats the console: a raised graphics
// clock does too, and it stays raised until the console reboots. so the watch runs
// whenever either is true, and stands down only when the console is back to idle
// at its boot clocks.
static int isConsoleBeingPushed(void)
{
   const LoadState *load = getLoadState();
   if (load->cpuLevel != LOAD_OFF || load->gpuLevel != LOAD_OFF) return 1;

   return getRsxCoreClockMhz() != getBootRsxCoreClockMhz() || getRsxMemoryClockMhz() != getBootRsxMemoryClockMhz();
}

void updateSafety(const Sensors *sensors)
{
   // raising a dial again is what re-arms us, so the notice clears itself
   const LoadState *load = getLoadState();
   if (cutoffFired && (load->cpuLevel != LOAD_OFF || load->gpuLevel != LOAD_OFF)) cutoffFired = 0;

   if (!isConsoleBeingPushed()) return;

   int cutoffTenths = getSafetyCutoffCelsius() * 10;
   int tooHot = sensors->cpuTenthsC >= cutoffTenths || sensors->rsxTenthsC >= cutoffTenths;
   if (sensors->temperatureReadable && !tooHot) return;

   if (sensors->temperatureReadable)
      logWarn("[bench] safety cutoff: cpu %d.%d C, rsx %d.%d C - dropping load to off\n",
              sensors->cpuTenthsC / 10, sensors->cpuTenthsC % 10, sensors->rsxTenthsC / 10, sensors->rsxTenthsC % 10);
   else
      logWarn("[bench] safety cutoff: the console refused to report its temperature - dropping load to off\n");

   setLoadOff();
   restoreBootRsxClocks();
   cutoffFired = 1;
}
