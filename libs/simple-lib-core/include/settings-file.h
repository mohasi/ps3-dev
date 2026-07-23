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

// does this line declare `key`? ("key = value", with spaces or tabs allowed around the '=')
static inline int lineDeclaresKey(const char *line, const char *key, int keyLength)
{
   if (!startsWith(line, key)) return 0;
   const char *after = line + keyLength;
   while (*after == ' ' || *after == '\t') after++;
   return *after == '=';
}

// the value part of the "key=value" line for `key`, or NULL when absent. spaces or tabs
// around the '=' are allowed, so lines can be aligned for readability ("key   = value"),
// and the returned value skips any leading spaces. the value runs to the end of its line;
// comment lines start with '#' and never match a key.
static inline const char *findSettingValue(const char *text, const char *key)
{
   int keyLength = getStrLen(key);
   for (const char *line = text; *line;) {
      if (lineDeclaresKey(line, key, keyLength)) {
         const char *after = line + keyLength;
         while (*after == ' ' || *after == '\t') after++;   // to the '='
         after++;
         while (*after == ' ' || *after == '\t') after++;
         return after;
      }
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

static inline void appendSettingLine(char *out, int cap, int *offset, const char *key, const char *value)
{
   appendStr(out, cap, offset, key);
   appendStr(out, cap, offset, "=");
   appendStr(out, cap, offset, value);
   appendStr(out, cap, offset, "\n");
}

#define SETTINGS_FILE_CAP 4096   // the largest settings file these helpers can rewrite without loss

// rewrites the file at path so that `key` reads `value`, or - when value is NULL - so that its line is
// gone. every other line (comments and blank lines included) is preserved, which is what lets several
// features share one settings.txt without clobbering each other; a key that isn't there yet is appended.
// value is written verbatim, so keep it single-line.
// returns 0 on success, -1 on a write failure or a file too big to rewrite without losing its tail.
static inline int rewriteSettingKey(const char *path, const char *key, const char *value)
{
   char text[SETTINGS_FILE_CAP];
   int length = readFile(path, text, sizeof text - 1);
   if (length < 0) length = 0;   // missing file -> start from empty
   if (length >= (int)sizeof text - 1) return -1;   // can't rewrite what we couldn't read whole
   text[length] = 0;
   if (length == 0 && !value) return 0;   // nothing to remove

   char out[SETTINGS_FILE_CAP];
   int written = 0;
   int keyLength = getStrLen(key);
   int found = 0;

   // copy the file line by line, swapping (or dropping) the matching key's line
   for (const char *line = text; *line;) {
      const char *lineEnd = line;
      while (*lineEnd && *lineEnd != '\n') lineEnd++;

      if (lineDeclaresKey(line, key, keyLength)) {
         found = 1;
         if (value) appendSettingLine(out, sizeof out, &written, key, value);
      } else {
         for (const char *c = line; c < lineEnd && written < (int)sizeof out - 1; c++) out[written++] = *c;
         if (written < (int)sizeof out - 1) out[written++] = '\n';
      }
      line = (*lineEnd) ? lineEnd + 1 : lineEnd;
   }

   if (!found) {
      if (!value) return 0;   // asked to delete a key that isn't there
      appendSettingLine(out, sizeof out, &written, key, value);
   }
   return writeFile(path, out, (uint64_t)written) == 0 ? 0 : -1;
}

// inserts or replaces the "key=value" line; removes it. both are one rewrite (see above).
static inline int upsertSettingValue(const char *path, const char *key, const char *value)
{
   return rewriteSettingKey(path, key, value);
}

static inline int deleteSettingKey(const char *path, const char *key)
{
   return rewriteSettingKey(path, key, NULL);
}
