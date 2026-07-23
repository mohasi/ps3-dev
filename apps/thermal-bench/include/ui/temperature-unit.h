#pragma once

// temperature-unit - which unit the screen shows. everything is measured, stored
// and logged in celsius; only what is displayed converts, so a run file is always
// comparable with any other whichever unit was on screen at the time.

void toggleTemperatureUnit(void);

const char *getTemperatureUnitText(void);   // "°C" or "°F"
int getDisplayTenths(int tenthsCelsius);    // converts if fahrenheit is selected
