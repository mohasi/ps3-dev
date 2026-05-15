#pragma once

// vsh plugin install / uninstall, expressed as wrappers over fileio.h.
//
// canonical layout:
//   /dev_hdd0/plugins/<name>.sprx        - plugin file
//   /dev_hdd0/boot_plugins.txt           - cobra/evilnat boot plugin manifest
//
//   install   = recvFile + setPluginLine(ACTIVE)
//   uninstall = setPluginLine(ABSENT) + deleteFile
//
// the manifest is a plain text file; one path per line. lines starting with
// '#' (after optional whitespace) are commented out. a line "matches" plugin
// <name> iff its trimmed-uncommented form ends with "/<name>.sprx" — this is
// path-agnostic so stale entries from other install locations also collapse.

#include <stdint.h>

#include "file.h"
#include "fileio.h"
#include "string-utilities.h"

#define BOOT_PLUGINS_PATH    "/dev_hdd0/boot_plugins.txt"
#define PLUGIN_DIR           "/dev_hdd0/plugins"
#define BOOT_PLUGINS_MAX     8192
#define PLUGIN_NAME_MAX      64

typedef enum {
    PLUGIN_LINE_ABSENT    = 0,
    PLUGIN_LINE_ACTIVE    = 1,
    PLUGIN_LINE_COMMENTED = 2
} PluginLineState;

// build canonical "/dev_hdd0/plugins/<name>.sprx" into out.
static inline void pluginPath(char *out, int cap, const char *name)
{
    int o = 0;
    appendStr(out, cap, &o, PLUGIN_DIR "/");
    appendStr(out, cap, &o, name);
    appendStr(out, cap, &o, ".sprx");
    if (o < cap) out[o] = '\0';
}

// 1 if [line .. line+lineLen) (after stripping leading ws + '#'s + trailing ws)
// ends with "/<name>.sprx" (case-insensitive). path-agnostic on purpose so
// duplicate entries with alternate prefixes get collapsed.
static int lineMatchesPlugin(const char *line, int lineLen, const char *name)
{
    int s = 0;
    while (s < lineLen && (line[s] == ' ' || line[s] == '\t')) s++;
    while (s < lineLen && line[s] == '#') s++;
    while (s < lineLen && (line[s] == ' ' || line[s] == '\t')) s++;
    int e = lineLen;
    while (e > s && (line[e-1] == ' ' || line[e-1] == '\t' || line[e-1] == '\r')) e--;

    char suffix[PLUGIN_NAME_MAX + 8];
    int so = 0;
    appendStr(suffix, sizeof suffix, &so, "/");
    appendStr(suffix, sizeof suffix, &so, name);
    appendStr(suffix, sizeof suffix, &so, ".sprx");

    if (so > e - s) return 0;
    const char *tail = line + e - so;
    for (int i = 0; i < so; i++) {
        char a = tail[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

// rewrite boot_plugins.txt: drop every line matching name, then optionally
// append a single canonical line. returns 0 on success.
static int setPluginLine(const char *name, PluginLineState state)
{
    static char in[BOOT_PLUGINS_MAX];
    static char out[BOOT_PLUGINS_MAX];

    int inLen = 0;
    if (fileExists(BOOT_PLUGINS_PATH)) {
        inLen = readFile(BOOT_PLUGINS_PATH, in, sizeof in);
        if (inLen < 0) inLen = 0;
    }

    int outOff = 0;
    int i = 0;
    while (i < inLen) {
        int j = i;
        while (j < inLen && in[j] != '\n') j++;
        int lineLen = j - i;
        if (!lineMatchesPlugin(in + i, lineLen, name)) {
            for (int k = i; k < j && outOff < (int)sizeof out - 1; k++) out[outOff++] = in[k];
            if (j < inLen && outOff < (int)sizeof out - 1) out[outOff++] = '\n';
        }
        i = j + 1;
    }

    if (state != PLUGIN_LINE_ABSENT) {
        if (outOff > 0 && out[outOff - 1] != '\n' && outOff < (int)sizeof out - 1) {
            out[outOff++] = '\n';
        }
        if (state == PLUGIN_LINE_COMMENTED) appendStr(out, sizeof out, &outOff, "# ");
        appendStr(out, sizeof out, &outOff, PLUGIN_DIR "/");
        appendStr(out, sizeof out, &outOff, name);
        appendStr(out, sizeof out, &outOff, ".sprx");
        if (outOff < (int)sizeof out - 1) out[outOff++] = '\n';
    }

    return writeFile(BOOT_PLUGINS_PATH, out, (uint64_t)outOff);
}

// install: recvFile -> canonical path, then enable in manifest.
static int pluginInstall(int cli, const char *name, uint32_t size)
{
    char path[FILE_PATH_MAX];
    pluginPath(path, sizeof path, name);
    makeDir(PLUGIN_DIR);
    if (recvFile(cli, path, size) < 0) return -1;
    return setPluginLine(name, PLUGIN_LINE_ACTIVE);
}

// uninstall: drop manifest entries, deleteFile (idempotent — missing file ok).
static int pluginUninstall(const char *name)
{
    char path[FILE_PATH_MAX];
    pluginPath(path, sizeof path, name);
    if (setPluginLine(name, PLUGIN_LINE_ABSENT) < 0) return -1;
    return deleteFile(path);
}
