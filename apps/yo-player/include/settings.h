#pragma once

// settings - user-editable options in /dev_hdd0/tmp/yo-player/settings.txt. the file is created
// with defaults and explanatory comments on first launch, so it can be edited over FTP; an unknown
// or malformed value silently falls back to its default. loadSettings() must run once at startup.

// when auto-skip acts on community-flagged SponsorBlock segments.
typedef enum {
   SPONSORBLOCK_OFF,   // never skip anything (the segments aren't even fetched)
   SPONSORBLOCK_ADS,   // skip paid promotions only: sponsors, self-promo, like/subscribe reminders
   SPONSORBLOCK_ALL    // also skip intros, outros, filler and non-music sections
} SponsorblockMode;

void loadSettings(void);   // reads settings.txt, creating it with defaults when missing

SponsorblockMode getSponsorblockMode(void);

int  getPreferredMaxHeight(void);        // video resolution ceiling for playback and download: 720 or 1080
void setPreferredMaxHeight(int height);  // set the choice in memory (720 or 1080); does not write to disk
void savePreferredMaxHeight(void);       // write a changed choice to settings.txt so it sticks next launch
