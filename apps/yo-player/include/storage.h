#pragma once

// storage - tiny persistent preferences + watch history, kept as plain text files under
// /dev_hdd0/tmp/yo-player/ (via the VFS layer). No user/account binding: back up or move the folder and
// anyone can restore it. initStorage() must run once at startup before the getters are used.

void initStorage(void);

// watch history: videoIds the user has played. Used to fade already-watched tiles in the grid.
int  isWatched(const char *videoId);
void markWatched(const char *videoId);

// resume: last playback position (seconds) per watched video, so playback picks up where it left off.
// getWatchedPosition returns 0 if unknown; a finished video is saved as 0 so it restarts next time.
int  getWatchedPosition(const char *videoId);
void setWatchedPosition(const char *videoId, int seconds);

#define CHANNEL_ID_LEN    32
#define MAX_SUBSCRIPTIONS 64

// subscribed channel ids, seeded to subscriptions.txt on first run (edit the file to change them; a proper
// subscribe UI comes later). fills ids[0..count-1], returns the count.
int  getSubscriptions(char ids[][CHANNEL_ID_LEN], int max);
