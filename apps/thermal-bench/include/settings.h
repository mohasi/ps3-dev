#pragma once

// settings - user-editable options in /dev_hdd0/tmp/thermal-bench/settings.txt. the file is
// created with defaults and explanatory comments on first launch, so it can be edited over FTP;
// an unknown or out-of-range value falls back to its default and says so in the log.
// loadSettings() must run once at startup, before the bench screen opens.

void loadSettings(void);

int getSafetyCutoffCelsius(void);   // the temperature the load switches itself off at
int isSafetyCutoffFromSettings(void);   // 1 when the settings file set it, 0 when the model chose it
