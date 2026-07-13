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

// real ps3 game ids start BC/BL (disc) or NP (psn); homebrew and apps don't. used to
// skip the "no cheats" toast / online lookup for non-games (they never have cheats).
int isGameTitleId(const char *titleId)
{
   return (titleId[0] == 'B' && (titleId[1] == 'C' || titleId[1] == 'L'))
       || (titleId[0] == 'N' && titleId[1] == 'P');
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
