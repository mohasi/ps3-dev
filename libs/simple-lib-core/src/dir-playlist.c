// dir-playlist - folder listing + prev/next-with-wrap navigation. See dir-playlist.h.
#include "dir-playlist.h"
#include "vfs.h"               // VFS helpers, joinPath, getParentPath, getBaseName, MAX_PATH_LEN
#include "string-utilities.h"   // strCopy, strCmpICase, strEq

int listDirFiltered(const char *dir, char names[][DIR_PLAYLIST_NAME_MAX], int maxCount, FileFilter accept)
{
   int count = 0;

   VfsDir d;
   if (openDir(dir, &d) != 0) return 0;

   char name[256];
   while (readDir(&d, name, sizeof name, NULL) == 1) {   // skips "." / ".."
      if (!accept(name)) continue;
      if (count >= maxCount) break;
      strCopy(names[count], DIR_PLAYLIST_NAME_MAX, name);
      count++;
   }
   closeDir(&d);

   // insertion sort, case-insensitive by name
   for (int i = 1; i < count; i++) {
      char key[DIR_PLAYLIST_NAME_MAX];
      strCopy(key, DIR_PLAYLIST_NAME_MAX, names[i]);
      int j = i - 1;
      while (j >= 0 && strCmpICase(names[j], key) > 0) {
         strCopy(names[j + 1], DIR_PLAYLIST_NAME_MAX, names[j]);
         j--;
      }
      strCopy(names[j + 1], DIR_PLAYLIST_NAME_MAX, key);
   }

   return count;
}

int playlistOpen(DirPlaylist *p, const char *path, FileFilter accept)
{
   getParentPath(path, p->dir, sizeof p->dir);
   p->count = listDirFiltered(p->dir, p->names, DIR_PLAYLIST_MAX, accept);

   const char *base = getBaseName(path);
   p->index = 0;
   for (int i = 0; i < p->count; i++)
      if (strEq(p->names[i], base)) { p->index = i; break; }

   if (p->count == 0) {   // path not in the listing: fall back to a single-entry list of just it
      strCopy(p->names[0], DIR_PLAYLIST_NAME_MAX, base);
      p->count = 1;
      p->index = 0;
   }
   return p->count;
}

void playlistStep(DirPlaylist *p, int delta, char *outPath, int outCap)
{
   if (p->count <= 0) { if (outCap > 0) outPath[0] = '\0'; return; }

   int n = p->index + delta;
   while (n < 0)        n += p->count;   // wrap past the start
   n %= p->count;                        // and past the end
   p->index = n;

   joinPath(outPath, outCap, p->dir, p->names[n]);
}
