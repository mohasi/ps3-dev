#include "ui/temperature-unit.h"

static int showFahrenheit;

void toggleTemperatureUnit(void) { showFahrenheit = !showFahrenheit; }

const char *getTemperatureUnitText(void) { return showFahrenheit ? "°F" : "°C"; }

int getDisplayTenths(int tenthsCelsius)
{
   return showFahrenheit ? tenthsCelsius * 9 / 5 + 320 : tenthsCelsius;
}
