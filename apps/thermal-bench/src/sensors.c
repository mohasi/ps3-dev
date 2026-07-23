#include <stdio.h>

#include "sensors.h"
#include "clocks.h"
#include "dbg.h"

static const char *FAN_MODE_NAMES[] = { "full", "automatic", "manual" };
#define FAN_MODE_NAME_COUNT (int)(sizeof FAN_MODE_NAMES / sizeof FAN_MODE_NAMES[0])

const char *getFanModeText(FanMode mode)
{
   return (int)mode < FAN_MODE_NAME_COUNT ? FAN_MODE_NAMES[mode] : "unknown";
}

static int readZoneTenths(ThermalZone zone, int *outTenthsC)
{
   uint32_t raw = 0;
   if (getConsoleTemperature(zone, &raw) != 0) return 0;

   *outTenthsC = getTemperatureCelsius(raw) * 10 + getTemperatureTenths(raw);
   return 1;
}

void readSensors(Sensors *out)
{
   Sensors blank = { 0 };
   *out = blank;

   // both zones must answer: a refused read looks exactly like 0 degrees, and
   // the safety cutoff must never mistake one for the other.
   int cpuRead = readZoneTenths(THERMAL_ZONE_CPU, &out->cpuTenthsC);
   int rsxRead = readZoneTenths(THERMAL_ZONE_RSX, &out->rsxTenthsC);
   out->temperatureReadable = cpuRead && rsxRead;

   FanPolicy fan;
   out->fanReadable = getFanPolicy(&fan) == 0;
   out->fanPercent  = out->fanReadable ? getFanPercent(fan.duty) : 0;
   out->fanMode     = (FanMode)fan.mode;

   out->coreClockMhz   = getRsxCoreClockMhz();
   out->memoryClockMhz = getRsxMemoryClockMhz();
}

void logSensorChanges(const Sensors *sensors)
{
   static Sensors previous;
   static int haveLogged;

   int changed = !haveLogged
              || sensors->cpuTenthsC / 10 != previous.cpuTenthsC / 10
              || sensors->rsxTenthsC / 10 != previous.rsxTenthsC / 10
              || sensors->temperatureReadable != previous.temperatureReadable
              || sensors->fanPercent != previous.fanPercent
              || sensors->fanMode    != previous.fanMode
              || sensors->coreClockMhz   != previous.coreClockMhz
              || sensors->memoryClockMhz != previous.memoryClockMhz;
   if (!changed) return;

   char temperature[48];
   if (sensors->temperatureReadable)
      snprintf(temperature, sizeof temperature, "cpu %d.%d C  rsx %d.%d C",
               sensors->cpuTenthsC / 10, sensors->cpuTenthsC % 10, sensors->rsxTenthsC / 10, sensors->rsxTenthsC % 10);
   else
      snprintf(temperature, sizeof temperature, "cpu/rsx unreadable");

   if (sensors->fanReadable)
      logInfo("[bench] %s  fan %d%% (%s)  core %d MHz  mem %d MHz\n", temperature,
              sensors->fanPercent, getFanModeText(sensors->fanMode), sensors->coreClockMhz, sensors->memoryClockMhz);
   else
      logInfo("[bench] %s  fan unreadable  core %d MHz  mem %d MHz\n", temperature,
              sensors->coreClockMhz, sensors->memoryClockMhz);

   previous = *sensors;
   haveLogged = 1;
}
