#pragma once

// cheat-sync - where cheat files come from and how the plugin syncs with the online repo.
// owns the on-console data paths, the local-file locator, the running game's version (from
// PARAM.SFO), the game-id filter, and the settings-driven sync mode. the online fetch/upload
// will live here too. rendering, parsing, and applying cheats are separate concerns (overlay.cpp).
//
// plain C (no PAF/C++ needs); the extern "C" guard lets the C++ overlay include it.

#ifdef __cplusplus
extern "C" {
#endif

// on-console data layout (mirrors what the user deploys): PLUGIN_DIR holds settings.txt and
// the cheats/ subdir of per-title <titleId>.txt files.
#define PLUGIN_DIR    "/dev_hdd0/tmp/simple-cheat-menu"
#define CHEATS_DIR    PLUGIN_DIR "/cheats/"
#define SETTINGS_PATH PLUGIN_DIR "/settings.txt"

// how far the plugin talks to the online cheat repo: offline = only local files; fetch =
// download cheats but stay silent; contribute = download + send anonymous feedback votes.
enum SyncMode { SYNC_OFFLINE, SYNC_FETCH, SYNC_CONTRIBUTE };
extern enum SyncMode syncMode;

void loadSyncMode(void);                                      // settings.txt -> syncMode (creates the data dirs + file if missing)
int  isGameTitleId(const char *titleId);                      // 1 for a real ps3 game id (BC/BL/NP), 0 for homebrew/apps
void buildCheatPath(char *out, int cap, const char *titleId); // CHEATS_DIR<titleId>.txt
int  getAppVersion(const char *titleId, char *out, int cap);  // APP_VER from PARAM.SFO; returns length written (0 if unavailable)

#ifdef __cplusplus
}
#endif
