// settings - load (and first-launch create) thermal-bench's settings.txt (see settings.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.
#include <stdlib.h>

#include "settings.h"
#include "console-model.h"
#include "settings-file.h"
#include "vfs.h"
#include "dbg.h"

#define SETTINGS_PATH "/dev_hdd0/tmp/thermal-bench/settings.txt"

// the ceiling is the modern default from console-model.h - nothing on this console
// tolerates more. the floor stops the load switching off before the console has
// warmed up at all.
#define CUTOFF_MIN_CELSIUS 50
#define CUTOFF_MAX_CELSIUS MODERN_CUTOFF_CELSIUS

static const char *DEFAULT_SETTINGS =
   "# thermal-bench settings - edit over FTP; changes apply on the next launch\n"
   "\n"
   "safety-cutoff=auto\n"
   "# the temperature (celsius) at which the load switches itself off and the clocks go back\n"
   "# to what they booted at. auto picks it from the console model: 70 on the launch consoles\n"
   "# up to CECHH, whose 90 nm graphics chip has weak solder underfill, and 88 on every later\n"
   "# console. a console we cannot identify is assumed to be one of the fragile ones and gets\n"
   "# 70. a number between 50 and 88 overrides all of that - use it if the graphics chip in\n"
   "# your console was swapped for a later one, or if you simply want to stop sooner.\n";

// the fragile default until loadSettings runs, so a caller that reads the cutoff too
// early gets the cautious number rather than 0, which would read as "always too hot".
static int safetyCutoffCelsius = FRAGILE_CUTOFF_CELSIUS;
static int cutoffFromSettings;

void loadSettings(void)
{
   safetyCutoffCelsius = getModelSafetyCutoffCelsius();

   makeDir("/dev_hdd0/tmp/thermal-bench");   // first launch: the folder has to exist before the file can be created

   char text[2048];
   if (loadSettingsFile(SETTINGS_PATH, DEFAULT_SETTINGS, text, sizeof text) == 1)
      logInfo("[bench] created %s with defaults\n", SETTINGS_PATH);

   const char *cutoff = findSettingValue(text, "safety-cutoff");
   if (!cutoff || settingValueEquals(cutoff, "auto")) return;

   int wanted = atoi(cutoff);
   if (wanted < CUTOFF_MIN_CELSIUS || wanted > CUTOFF_MAX_CELSIUS)
   {
      logWarn("[bench] safety-cutoff %d is outside %d-%d C, using %d C\n",
              wanted, CUTOFF_MIN_CELSIUS, CUTOFF_MAX_CELSIUS, safetyCutoffCelsius);
      return;
   }

   safetyCutoffCelsius = wanted;
   cutoffFromSettings = 1;
   logInfo("[bench] safety cutoff set to %d C by settings.txt\n", safetyCutoffCelsius);
}

int getSafetyCutoffCelsius(void)   { return safetyCutoffCelsius; }
int isSafetyCutoffFromSettings(void) { return cutoffFromSettings; }
