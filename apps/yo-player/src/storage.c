// storage - plain-text preferences + watch history under /dev_hdd0/tmp/yo-player/ (see storage.h).

#include "storage.h"
#include "vfs.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DATA_DIR     "/dev_hdd0/tmp/yo-player"
#define HISTORY_PATH DATA_DIR "/history.txt"
#define SUBS_PATH    DATA_DIR "/subscriptions.txt"

#define HISTORY_CAP    1000     // watched videos kept (oldest dropped past this)
#define VIDEO_ID_LEN   12       // 11 chars + NUL
#define ENTRY_LINE_MAX 24       // "videoId seconds\n"
#define HISTORY_BUFFER_BYTES (HISTORY_CAP * ENTRY_LINE_MAX + 16)   // whole-file load/save buffer

// watch history held in memory; the file is the durable copy. a plain ring: once full, the oldest slot is
// overwritten (rare - 1000 videos), and the file is rewritten from the live set. each entry also carries the
// last playback position for resume.
typedef struct { char id[VIDEO_ID_LEN]; int position; } WatchEntry;
static WatchEntry watched[HISTORY_CAP];
static int        watchedCount;
static int        watchedNext;  // next slot to write when the ring is full

static void loadHistory(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = readFile(HISTORY_PATH, buffer, HISTORY_BUFFER_BYTES - 1);
   if (length > 0) {
      buffer[length] = 0;
      for (char *line = strtok(buffer, "\r\n"); line && watchedCount < HISTORY_CAP; line = strtok(NULL, "\r\n")) {
         char id[VIDEO_ID_LEN]; int position = 0;
         if (line[0] && sscanf(line, "%11s %d", id, &position) >= 1) {   // second field optional (old files)
            strCopy(watched[watchedCount].id, VIDEO_ID_LEN, id);
            watched[watchedCount].position = position;
            watchedCount++;
         }
      }
   }
   free(buffer);
}

// a resumable key is a bare videoId; a typed url (carries '/' or ':') never enters history.
static int isHistoryKey(const char *videoId) { return videoId[0] && !strchr(videoId, '/') && !strchr(videoId, ':'); }

static WatchEntry *findWatched(const char *videoId)
{
   for (int i = 0; i < watchedCount; i++)
      if (strcmp(watched[i].id, videoId) == 0) return &watched[i];
   return NULL;
}

// add a fresh entry (ring-overwrites the oldest when full) at position 0.
static WatchEntry *addWatched(const char *videoId)
{
   WatchEntry *entry;
   if (watchedCount < HISTORY_CAP) entry = &watched[watchedCount++];
   else { entry = &watched[watchedNext]; watchedNext = (watchedNext + 1) % HISTORY_CAP; }
   strCopy(entry->id, VIDEO_ID_LEN, videoId);
   entry->position = 0;
   return entry;
}

// seeded on first run; the user edits subscriptions.txt to change these. one channel id per line.
static const char *DEFAULT_SUBS =
   "UCX6OQ3DkcsbYNE6H8uQQuVA\n"   // MrBeast
   "UCXuqSBlHAE6Xw-yeJA0Tunw\n"   // Linus Tech Tips
   "UCBJycsmduvYEL83R_U4JriQ\n"   // MKBHD
   "UCHnyfMqiRRG1u-2MsSQLbXA\n"   // Veritasium
   "UCsXVk37bltHxD1rDPwtNM8Q\n";  // Kurzgesagt

static void seedSubscriptions(void)
{
   if (!fileExists(SUBS_PATH)) writeFile(SUBS_PATH, DEFAULT_SUBS, strlen(DEFAULT_SUBS));
}

void initStorage(void)
{
   makeDir(DATA_DIR);
   loadHistory();
   seedSubscriptions();
}

int getSubscriptions(char ids[][CHANNEL_ID_LEN], int max)
{
   char buffer[MAX_SUBSCRIPTIONS * CHANNEL_ID_LEN + 16];
   int length = readFile(SUBS_PATH, buffer, sizeof buffer - 1);
   if (length <= 0) return 0;
   buffer[length] = 0;

   int count = 0;
   for (char *line = strtok(buffer, "\r\n"); line && count < max; line = strtok(NULL, "\r\n"))
      if (line[0] == 'U' && line[1] == 'C') { strCopy(ids[count], CHANNEL_ID_LEN, line); count++; }   // skip blanks/comments
   return count;
}

// rewrite subscriptions.txt from an id array (one channel id per line).
static void saveSubscriptions(char ids[][CHANNEL_ID_LEN], int count)
{
   char buffer[MAX_SUBSCRIPTIONS * CHANNEL_ID_LEN + 16];
   int length = 0;
   for (int i = 0; i < count; i++)
      length += snprintf(buffer + length, CHANNEL_ID_LEN + 2, "%s\n", ids[i]);
   writeFile(SUBS_PATH, buffer, length);
}

static int subsRevision;   // bumped on every real subscribe/unsubscribe so cached feeds know to refetch
int getSubscriptionsRevision(void) { return subsRevision; }

int isSubscribed(const char *channelId)
{
   char ids[MAX_SUBSCRIPTIONS][CHANNEL_ID_LEN];
   int count = getSubscriptions(ids, MAX_SUBSCRIPTIONS);
   for (int i = 0; i < count; i++)
      if (strcmp(ids[i], channelId) == 0) return 1;
   return 0;
}

void setSubscribed(const char *channelId, int subscribed)
{
   if (channelId[0] != 'U' || channelId[1] != 'C') return;   // only real UC ids
   char ids[MAX_SUBSCRIPTIONS][CHANNEL_ID_LEN];
   int count = getSubscriptions(ids, MAX_SUBSCRIPTIONS);
   int found = -1;
   for (int i = 0; i < count; i++)
      if (strcmp(ids[i], channelId) == 0) { found = i; break; }

   if (subscribed && found < 0) {
      if (count >= MAX_SUBSCRIPTIONS) return;
      strCopy(ids[count++], CHANNEL_ID_LEN, channelId);
   } else if (!subscribed && found >= 0) {
      for (int i = found; i < count - 1; i++) strCopy(ids[i], CHANNEL_ID_LEN, ids[i + 1]);
      count--;
   } else {
      return;   // already in the desired state
   }
   saveSubscriptions(ids, count);
   subsRevision++;
}

int isWatched(const char *videoId) { return findWatched(videoId) != NULL; }

int getWatchedPosition(const char *videoId)
{
   WatchEntry *entry = findWatched(videoId);
   return entry ? entry->position : 0;
}

// rewrite history.txt from the in-memory set (one "id position" per line).
static void saveHistory(void)
{
   char *buffer = malloc(HISTORY_BUFFER_BYTES);
   if (!buffer) return;
   int length = 0;
   for (int i = 0; i < watchedCount; i++)
      length += snprintf(buffer + length, ENTRY_LINE_MAX + 1, "%s %d\n", watched[i].id, watched[i].position);
   writeFile(HISTORY_PATH, buffer, length);
   free(buffer);
}

void markWatched(const char *videoId)
{
   if (!isWatched(videoId)) setWatchedPosition(videoId, 0);   // add new at position 0; keep an existing position
}

void setWatchedPosition(const char *videoId, int seconds)
{
   if (!isHistoryKey(videoId)) return;
   WatchEntry *entry = findWatched(videoId);
   if (!entry) entry = addWatched(videoId);
   entry->position = seconds;
   saveHistory();
}
