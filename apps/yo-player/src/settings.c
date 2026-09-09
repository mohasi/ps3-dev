// settings - load (and first-launch create) yo-player's settings.txt (see settings.h).
// the file format + parsing is the shared simple-lib-core settings-file.h standard.

#include "settings.h"
#include "settings-file.h"
#include "dbg.h"

#define SETTINGS_PATH "/dev_hdd0/tmp/yo-player/settings.txt"

// the file written on first launch: each setting at its default, with a comment explaining the values.
static const char *DEFAULT_SETTINGS =
   "# yo-player settings - edit over FTP; changes apply on the next launch\n"
   "\n"
   "sponsorblock-mode=ads\n"
   "# off = never skip anything, ads = skip paid promotions only (sponsors, self-promo,\n"
   "# like/subscribe reminders), all = also skip intros, outros, filler and non-music sections\n"
   "\n"
   "theme=youtube\n"
   "# which colour theme to start in - one of the [Name] blocks in themes.txt (lower case,\n"
   "# spaces become hyphens). the shipped one is youtube.\n"
   "\n"
   "resolution=720p\n"
   "# preferred video resolution, 720p or 1080p. Square toggles it during playback and the choice\n"
   "# is saved here. 1080p looks better but buffers more on a slow connection.\n";

static SponsorblockMode sponsorblockMode = SPONSORBLOCK_ADS;
static int preferredMaxHeight = 720;
static int preferredMaxHeightDirty = 0;   // changed in memory but not yet written to disk

void loadSettings(void)
{
   char text[2048];
   if (loadSettingsFile(SETTINGS_PATH, DEFAULT_SETTINGS, text, sizeof text) == 1)
      logInfo("[settings] created %s with defaults\n", SETTINGS_PATH);

   const char *mode = findSettingValue(text, "sponsorblock-mode");
   if (mode) {
      if      (settingValueEquals(mode, "off")) sponsorblockMode = SPONSORBLOCK_OFF;
      else if (settingValueEquals(mode, "ads")) sponsorblockMode = SPONSORBLOCK_ADS;
      else if (settingValueEquals(mode, "all")) sponsorblockMode = SPONSORBLOCK_ALL;
      else logWarn("[settings] unknown sponsorblock-mode value, using ads\n");
   }

   const char *resolution = findSettingValue(text, "resolution");
   if (resolution) preferredMaxHeight = settingValueEquals(resolution, "1080p") ? 1080 : 720;
}

SponsorblockMode getSponsorblockMode(void) { return sponsorblockMode; }

int getPreferredMaxHeight(void) { return preferredMaxHeight; }

// keep the choice in memory during playback; savePreferredMaxHeight writes it on the way out, so a
// resolution switch does not touch the filesystem on every Square press.
void setPreferredMaxHeight(int height)
{
   int clamped = height >= 1080 ? 1080 : 720;
   if (clamped != preferredMaxHeight) { preferredMaxHeight = clamped; preferredMaxHeightDirty = 1; }
}

void savePreferredMaxHeight(void)
{
   if (!preferredMaxHeightDirty) return;
   rewriteSettingKey(SETTINGS_PATH, "resolution", preferredMaxHeight == 1080 ? "1080p" : "720p");
   preferredMaxHeightDirty = 0;
}
