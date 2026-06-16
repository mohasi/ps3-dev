#pragma once

// Where the current game and its saves live. At startup the player scans RENPY_ROOT
// (/dev_hdd0/renpy) for a .rpk and adopts the first one it finds; if none is present it falls back
// to the dev bundle in USRDIR. Saves for a game go in RENPY_ROOT/<rpk-name>/, created on first save.
// Resolution is lazy + cached, so any getter can be the first call.

const char *getGameRpkPath(void);   // full path to the loaded .rpk
const char *getGameName(void);      // the .rpk base name (no dir, no extension) -- also the save-folder name
const char *getGameSaveDir(void);   // RENPY_ROOT/<getGameName> : the folder this game's slots are written to

// Multi-game picker (home screen): enumerate every .rpk in RENPY_ROOT and let the user choose one.
#define GAME_NAME_MAX 128   // max .rpk filename length
#define GAME_LIST_MAX 32    // max games listed at once

// Fills `out` with the bare .rpk filenames (with extension) in RENPY_ROOT. Returns the count (<=cap).
int  listGames(char out[][GAME_NAME_MAX], int cap);
// Adopts `file` (a bare .rpk filename in RENPY_ROOT) as the current game (sets rpk path/name/saveDir).
// Call before entering the play screen; overrides the lazy first-.rpk default.
void selectGame(const char *file);
