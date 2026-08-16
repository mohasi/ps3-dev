#include "cheat-sync.h"
#include "string-utilities.h"   // strCopy, getStrLen, appendStr, strCmpICase
#include "sfo.h"                // getSfoValue

#include <stdint.h>

// prx-safe vfs primitives (declared here so this TU stays clear of the heavier vfs.h).
extern int readFile(const char *path, char *buffer, int capacity);
extern int writeFile(const char *path, const char *data, uint64_t len);
extern int makeDirPath(const char *path);   // single-level mkdir (NOT recursive)

enum SyncMode syncMode = SYNC_CONTRIBUTE;

// full expected path for a title's cheat file: CHEATS_DIR<titleId>.txt.
void buildCheatPath(char *out, int cap, const char *titleId)
{
   strCopy(out, cap, CHEATS_DIR);
   int end = getStrLen(out);
   for (int i = 0; titleId[i] && end < cap - 5; i++) out[end++] = titleId[i];
   strCopy(out + end, cap - end, ".txt");
}

// the running game's version, from PARAM.SFO. the update/PSN install dir carries the
// patched version; an unpatched disc has none there, so fall back to the disc's own
// PARAM.SFO. out is left empty when neither is readable. (fallback chain IS detection.)
int getAppVersion(const char *titleId, char *out, int cap)
{
   out[0] = '\0';
   char path[64];
   int end = 0;
   appendStr(path, sizeof(path), &end, "/dev_hdd0/game/");
   appendStr(path, sizeof(path), &end, titleId);
   appendStr(path, sizeof(path), &end, "/PARAM.SFO");
   path[end] = '\0';

   unsigned char sfo[4096];
   int bytes = readFile(path, (char *)sfo, sizeof(sfo));
   if (bytes <= 0) bytes = readFile("/dev_bdvd/PS3_GAME/PARAM.SFO", (char *)sfo, sizeof(sfo));
   if (bytes <= 0) return 0;
   int written = getSfoValue(sfo, (uint64_t)bytes, "APP_VER", out, cap);
   return written > 0 ? written : 0;
}

// the prefixes a real ps3 game's id starts with: BL/BC are disc releases, NP is psn, and the
// rest are publisher ranges (Koei, Kadokawa, Marvelous, Asia region, Gust).
static const char *gameIdPrefixes[] = { "BL", "BC", "NP", "KOEI3", "KTGS3", "MRTC0", "ASIA0", "GUST0" };

// homebrew and tools that borrow a game-shaped id, so the prefix test alone lets them through.
// BLES806xx is the range homebrew packagers use (webMAN, IRISMAN and friends).
static const char *toolIdPrefixes[] = { "BLES806" };

// is this the id of a real game, as opposed to homebrew or a tool? The menu opens for a game
// whether or not it has any cheats, and stays shut for everything else.
int isGameTitleId(const char *titleId)
{
   for (unsigned int i = 0; i < sizeof(toolIdPrefixes) / sizeof(toolIdPrefixes[0]); i++)
      if (startsWith(titleId, toolIdPrefixes[i])) return 0;

   // a psn id's third character is its region letter (NPEA, NPUB, ...). Homebrew that squats on
   // the NP prefix puts a digit there instead, which is one rule rather than a list to maintain.
   if (startsWith(titleId, "NP") && (titleId[2] < 'A' || titleId[2] > 'Z')) return 0;

   for (unsigned int i = 0; i < sizeof(gameIdPrefixes) / sizeof(gameIdPrefixes[0]); i++)
      if (startsWith(titleId, gameIdPrefixes[i])) return 1;
   return 0;
}

// The stats counter's rows share settings.txt with the sync mode. Each is one "key=0" or "key=1"
// line; a key that is missing keeps its default, so an older file still loads.
static const char *statsKeys[] = { "stats", "statsGraph", "statsClocks", "statsTemps", "statsRight" };
#define STATS_KEY_COUNT  (int)(sizeof(statsKeys) / sizeof(statsKeys[0]))

// the value after "<key>=" on its own line, or -1 if the key is not in the text
static int readSettingValue(const char *text, const char *key)
{
   for (int at = 0; text[at]; at++) {
      if (at != 0 && text[at - 1] != '\n') continue;
      int k = 0;
      while (key[k] && text[at + k] == key[k]) k++;
      if (key[k] || text[at + k] != '=') continue;
      return text[at + k + 1] == '0' ? 0 : 1;
   }
   return -1;
}

void loadStatsSettingsFromFile(int *enabled, int *showGraph, int *showClocks, int *showTemps, int *topRight)
{
   int *values[STATS_KEY_COUNT] = { enabled, showGraph, showClocks, showTemps, topRight };

   char text[512];
   int bytes = readFile(SETTINGS_PATH, text, sizeof(text) - 1);
   if (bytes <= 0) return;   // no file yet: every row keeps its default
   text[bytes] = '\0';

   for (int i = 0; i < STATS_KEY_COUNT; i++) {
      int value = readSettingValue(text, statsKeys[i]);
      if (value >= 0) *values[i] = value;
   }
}

// Rewrites the whole file, sync mode included, since there is no in-place edit for one line.
void saveStatsSettings(int enabled, int showGraph, int showClocks, int showTemps, int topRight)
{
   int values[STATS_KEY_COUNT] = { enabled, showGraph, showClocks, showTemps, topRight };

   char text[512];
   int end = 0;
   appendStr(text, sizeof(text), &end, "mode=");
   appendStr(text, sizeof(text), &end, syncMode == SYNC_OFFLINE ? "offline" : syncMode == SYNC_FETCH ? "fetch" : "contribute");
   appendStr(text, sizeof(text), &end, "\n");

   for (int i = 0; i < STATS_KEY_COUNT; i++) {
      appendStr(text, sizeof(text), &end, statsKeys[i]);
      appendStr(text, sizeof(text), &end, values[i] ? "=1\n" : "=0\n");
   }

   writeFile(SETTINGS_PATH, text, (uint64_t)end);
}

// read the "mode=" value from settings.txt into syncMode. no file -> default to
// contribute and write it out, so the file exists for the user to change later.
void loadSyncMode(void)
{
   // create the data dirs before reading/writing them. mkdir is NOT recursive, so make
   // the parent first, then the cheats subdir (both idempotent - already-exists is fine).
   static int dirsEnsured = 0;
   if (!dirsEnsured) { makeDirPath(PLUGIN_DIR); makeDirPath(PLUGIN_DIR "/cheats"); dirsEnsured = 1; }

   syncMode = SYNC_CONTRIBUTE;
   char text[128];
   int bytes = readFile(SETTINGS_PATH, text, sizeof(text) - 1);
   if (bytes <= 0) { writeFile(SETTINGS_PATH, "mode=contribute\n", 16); return; }
   text[bytes] = '\0';

   // find "mode=" at a line start, then copy the value word (stop at any whitespace).
   for (int at = 0; text[at]; at++) {
      int lineStart = (at == 0 || text[at - 1] == '\n');
      if (lineStart && text[at] == 'm' && text[at + 1] == 'o' && text[at + 2] == 'd' && text[at + 3] == 'e' && text[at + 4] == '=') {
         char value[16];
         int v = 0, s = at + 5;
         while (text[s] && text[s] != '\n' && text[s] != '\r' && text[s] != ' ' && text[s] != '\t' && v < (int)sizeof(value) - 1)
            value[v++] = text[s++];
         value[v] = '\0';
         if (strCmpICase(value, "offline") == 0)      syncMode = SYNC_OFFLINE;
         else if (strCmpICase(value, "fetch") == 0)   syncMode = SYNC_FETCH;
         else                                         syncMode = SYNC_CONTRIBUTE;   // "contribute" or anything unknown
         return;
      }
   }
}
