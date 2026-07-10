#pragma once

// storage - tiny persistent preferences + watch history, kept as plain text files under
// /dev_hdd0/tmp/yo-player/ (via the VFS layer). No user/account binding: back up or move the folder and
// anyone can restore it. initStorage() must run once at startup before the getters are used.

#include "extractor.h"   // SearchResult / SearchResults (watch-later stores full entries)

void initStorage(void);

// watch history: videoIds the user has played. Used to fade already-watched tiles in the grid.
int  isWatched(const char *videoId);
void markWatched(const char *videoId);

// resume: last playback position (seconds) per watched video, so playback picks up where it left off.
// getWatchedPosition returns 0 if unknown; a finished video is saved as 0 so it restarts next time.
int  getWatchedPosition(const char *videoId);
void setWatchedPosition(const char *videoId, int seconds);

#define CHANNEL_ID_LEN    32
#define MAX_SUBSCRIPTIONS 128

// subscribed channel ids, seeded to subscriptions.txt on first run. fills ids[0..count-1], returns the count.
int  getSubscriptions(char ids[][CHANNEL_ID_LEN], int max);

// subscribe toggle: isSubscribed reports the current state; setSubscribed adds or removes the channel and
// rewrites subscriptions.txt immediately. Only real "UC..." ids are stored; duplicates never occur.
int  isSubscribed(const char *channelId);
void setSubscribed(const char *channelId, int subscribed);

// bumped whenever setSubscribed actually changes the set. Screens that cache the subscriptions feed compare
// this against the value they last loaded at, and refetch when it moved.
int  getSubscriptionsRevision(void);

// watch-later: a saved queue of full video entries (watchlater.txt), also shown as its own home category.
// isWatchLater powers the tile badge; toggleWatchLater queues the video (its metadata is kept so the
// category renders without a re-fetch) or removes it if already queued; getWatchLater fills a feed
// newest-first. getWatchLaterRevision bumps on every real change so cached feeds know to refetch.
int  isWatchLater(const char *videoId);
void toggleWatchLater(const SearchResult *item);
int  getWatchLater(SearchResults *out);
int  getWatchLaterRevision(void);
