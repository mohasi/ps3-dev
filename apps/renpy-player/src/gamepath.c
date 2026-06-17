#include "gamepath.h"

#include <string.h>

#include "config.h"   // RENPY_ROOT
#include "printf.h"   // snprintf
#include "file.h"     // cellFsOpendir/Readdir + CellFsDirent + MAX_PATH_LEN
#include "dbg.h"      // logInfo

static char rpkPath[MAX_PATH_LEN];
static char name[128];
static char saveDir[MAX_PATH_LEN];
static int  resolved;

static int hasRpkSuffix(const char *s)
{
   int n = (int)strlen(s);
   return n > 4 && (strcmp(s + n - 4, ".rpk") == 0 || strcmp(s + n - 4, ".RPK") == 0);
}

// Find the first *.rpk in RENPY_ROOT. Returns 1 and fills `out` with the bare filename, else 0.
static int findRpk(char *out, int cap)
{
   int fd;
   if (cellFsOpendir(RENPY_ROOT, &fd) != CELL_FS_SUCCEEDED) return 0;
   CellFsDirent ent; uint64_t got; int found = 0;
   while (!found && cellFsReaddir(fd, &ent, &got) == CELL_FS_SUCCEEDED && got > 0)
      if (ent.d_name[0] != '.' && hasRpkSuffix(ent.d_name)) { snprintf(out, cap, "%s", ent.d_name); found = 1; }
   cellFsClosedir(fd);
   return found;
}

static void resolve(void)
{
   if (resolved) return;
   resolved = 1;

   char file[128];
   if (findRpk(file, sizeof file))
   {
      snprintf(rpkPath, sizeof rpkPath, "%s/%s", RENPY_ROOT, file);
      int n = (int)strlen(file) - 4;                 // strip ".rpk"
      if (n > (int)sizeof name - 1) n = sizeof name - 1;
      memcpy(name, file, n); name[n] = '\0';
   }
   else
   {
      rpkPath[0] = '\0';   // no .rpk present: callers fail gracefully, the home picker shows empty
      name[0] = '\0';
   }
   snprintf(saveDir, sizeof saveDir, "%s/%s", RENPY_ROOT, name);
   logInfo("[rpp] game: %s  (saves -> %s)\n", rpkPath, saveDir);
}

const char *getGameRpkPath(void) { resolve(); return rpkPath; }
const char *getGameName(void)    { resolve(); return name; }
const char *getGameSaveDir(void) { resolve(); return saveDir; }

int listGames(char out[][GAME_NAME_MAX], int cap)
{
   int fd, count = 0;
   if (cap <= 0) return 0;
   if (cellFsOpendir(RENPY_ROOT, &fd) != CELL_FS_SUCCEEDED) return 0;
   CellFsDirent ent; uint64_t got;
   while (count < cap && cellFsReaddir(fd, &ent, &got) == CELL_FS_SUCCEEDED && got > 0)
      if (ent.d_name[0] != '.' && hasRpkSuffix(ent.d_name))
         snprintf(out[count++], GAME_NAME_MAX, "%s", ent.d_name);
   cellFsClosedir(fd);
   return count;
}

void selectGame(const char *file)
{
   if (!file || !file[0]) return;
   resolved = 1;   // pin this choice so the lazy resolve() never overrides it
   snprintf(rpkPath, sizeof rpkPath, "%s/%s", RENPY_ROOT, file);
   int n = (int)strlen(file);
   if (hasRpkSuffix(file)) n -= 4;                 // strip ".rpk" for the base name / save folder
   if (n > (int)sizeof name - 1) n = sizeof name - 1;
   memcpy(name, file, n); name[n] = '\0';
   snprintf(saveDir, sizeof saveDir, "%s/%s", RENPY_ROOT, name);
   logInfo("[rpp] selected game: %s  (saves -> %s)\n", rpkPath, saveDir);
}
