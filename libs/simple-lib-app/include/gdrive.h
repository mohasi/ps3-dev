#pragma once
//
// gdrive.h - Google Drive VFS backend for apps (file-manager).
//
// Surfaces the user's Google Drive as a virtual mount "/Google Drive" at the VFS
// root, beside the real storage devices. It is an app-side backend (it uses http.h
// + a heap), so it lives in simple-lib-app and is pulled ONLY by binaries that call
// initGdrive - VSH plugins that route paths never drag it in.
//
// The mount appears only once credentials are present (plain or encrypted) - an unconfigured console
// shows no Google Drive folder at all. Full-Drive access needs the browser consent flow, which the
// console has no browser for and the on-console device flow is not allowed to grant - so the refresh
// token is obtained ONCE on a PC (see dev/tools/get-gdrive-token.ps1) and pasted into settings.txt as
// google_client_id / google_client_secret / google_refresh_token. On the first successful connect the
// three are encrypted into a console-bound "google_auth_enc" blob and the plaintext keys are stripped;
// pasting fresh plaintext keys later overrides that blob, which is how a dead token is recovered from.
//
// Requires an http transport to be up first (initModernHttp / BearSSL reaches Google).
//
void initGdrive(const char *settingsPath);   // read settings + seed keys; mounts /Google Drive if configured
void shutdownGdrive(void);                   // unmount /Google Drive and release state

int isGdriveAuthorized(void);          // client id + secret + refresh token all present (connect can be tried)
int isGdrivePath(const char *path);    // 1 for "/Google Drive" or anything inside it (the app badges it)
