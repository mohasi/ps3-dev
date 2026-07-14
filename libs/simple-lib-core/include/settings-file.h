#pragma once

// settings-file - the standard user-editable settings file shared by the apps: one "key=value"
// per line, '#' starts a comment line, unknown keys and values are ignored (callers fall back to
// their defaults). Each app creates its file with documented defaults on first launch so it can
// be edited over FTP. libc-free (prx-safe), like the rest of simple-lib-core.

#include "vfs.h"                // readFile / writeFile
#include "string-utilities.h"   // getStrLen, startsWith, strCopy

// reads the settings file into out (NUL-terminated). when the file is missing, writes `defaults`
// to it and serves those. returns 0 = loaded, 1 = just created with defaults, -1 = no file and
// the create failed (out still holds the defaults, so the caller behaves normally either way).
static inline int loadSettingsFile(const char *path, const char *defaults, char *out, int cap)
{
   int length = readFile(path, out, cap - 1);
   if (length > 0) { out[length] = 0; return 0; }
   strCopy(out, cap, defaults);
   return writeFile(path, defaults, (uint64_t)getStrLen(defaults)) == 0 ? 1 : -1;
}

// the value part of the "key=value" line for `key`, or NULL when absent. the value runs to the
// end of its line; comment lines start with '#' and never match a key.
static inline const char *findSettingValue(const char *text, const char *key)
{
   int keyLength = getStrLen(key);
   for (const char *line = text; *line;) {
      if (startsWith(line, key) && line[keyLength] == '=') return line + keyLength + 1;
      while (*line && *line != '\n') line++;
      if (*line) line++;
   }
   return NULL;
}

// does a value returned by findSettingValue equal `expected`? (values run to the end of their
// line, so a whole-string compare would take the rest of the file with them.) a trailing space
// also terminates the value, leaving room for an inline comment after it.
static inline int settingValueEquals(const char *value, const char *expected)
{
   int length = getStrLen(expected);
   for (int i = 0; i < length; i++) if (value[i] != expected[i]) return 0;
   return value[length] == 0 || value[length] == '\n' || value[length] == '\r' || value[length] == ' ';
}
