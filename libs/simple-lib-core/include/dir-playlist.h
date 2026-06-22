#pragma once

// dir-playlist - browse a folder of matching files with prev/next-and-wrap navigation.
// Shared by media viewers/players (image viewer, audio player, future video player): each passes a
// file-type predicate and gets back a sorted list plus a current position it can step through.

#include "vfs.h"   // MAX_PATH_LEN

#define DIR_PLAYLIST_MAX       512   // max files tracked in one folder
#define DIR_PLAYLIST_NAME_MAX  256   // max bytes (incl NUL) per filename

// predicate deciding whether a filename belongs in the list (e.g. isPlayableAudioFile).
typedef int (*FileFilter)(const char *name);

// Lists files in `dir` accepted by `accept` into `names`, sorted case-insensitively, capped at
// maxCount. Returns the count. (Low-level helper; most callers want a DirPlaylist instead.)
int listDirFiltered(const char *dir, char names[][DIR_PLAYLIST_NAME_MAX], int maxCount, FileFilter accept);

// A folder of matching files with a current position.
typedef struct {
   char dir[MAX_PATH_LEN];
   char names[DIR_PLAYLIST_MAX][DIR_PLAYLIST_NAME_MAX];   // sorted, filtered
   int  count;
   int  index;                                            // current entry
} DirPlaylist;

// Scans the folder containing `path` (filtering by `accept`) and points `index` at `path`'s entry.
// If `path` isn't in the listing (e.g. it changed underneath us), falls back to a one-entry list of
// just that file. Returns the entry count.
int playlistOpen(DirPlaylist *p, const char *path, FileFilter accept);

// Steps `index` by `delta`, wrapping at both ends, and writes the new entry's full path (dir + name)
// into `outPath` (up to `outCap` bytes).
void playlistStep(DirPlaylist *p, int delta, char *outPath, int outCap);
