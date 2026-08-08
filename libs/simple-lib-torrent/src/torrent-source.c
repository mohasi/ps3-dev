#include "torrent-source.h"

#include "dbg.h"
#include "sha1.h"
#include "string-utilities.h"
#include "vfs.h"

#define TAG "[bt] "

#define SOURCE_FILE_MAX 2048
#define DISABLED_FOLDER "disabled"

static const char *skipSpaces(const char *text)
{
   while (*text == ' ' || *text == '\t') text++;
   return text;
}

// copy to the end of the line, without the trailing spaces
static void copyLineValue(char *out, int capacity, const char *value)
{
   int length = 0;
   while (value[length] && value[length] != '\n' && value[length] != '\r' && length < capacity - 1) length++;
   while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t')) length--;

   memCopy(out, value, length);
   out[length] = 0;
}

// "key = value" on this line, or 0
static const char *readKeyValue(const char *line, const char *key)
{
   const char *after = skipSpaces(line);
   int keyLength = getStrLen(key);

   for (int index = 0; index < keyLength; index++)
      if (after[index] != key[index]) return 0;

   after = skipSpaces(after + keyLength);
   if (*after != '=') return 0;

   return skipSpaces(after + 1);
}

// the file's own name, without the extension, for a source that does not name itself
static void copyNameFromFile(char *out, int capacity, const char *fileName)
{
   int length = 0;
   while (fileName[length] && fileName[length] != '.' && length < capacity - 1) length++;

   memCopy(out, fileName, length);
   out[length] = 0;
}

// a run of digits, from at, leaving at just past them
static int readNumber(const char *text, int *at)
{
   int value = 0;
   while (text[*at] >= '0' && text[*at] <= '9') value = value * 10 + (text[(*at)++] - '0');

   return value;
}

// One file, one source. Returns 0 when it describes a usable one, -1 when it does not, which covers
// a file that is not a source at all as well as one this build cannot read.
static int readSourceFile(TorrentSource *source, const char *directory, const char *fileName)
{
   char path[MAX_PATH_LEN];
   joinPath(path, sizeof path, directory, fileName);

   char text[SOURCE_FILE_MAX];
   if (readFile(path, text, sizeof text) < 0) return -1;

   memSet(source, 0, sizeof *source);
   strCopy(source->fileName, SOURCE_NAME_MAX, fileName);
   copyNameFromFile(source->name, SOURCE_NAME_MAX, fileName);
   source->enabled = 1;

   for (const char *line = text; *line;) {
      const char *value = 0;

      if ((value = readKeyValue(line, "name")) != 0) copyLineValue(source->name, SOURCE_NAME_MAX, value);
      else if ((value = readKeyValue(line, "search")) != 0) copyLineValue(source->searchUrl, SOURCE_URL_MAX, value);
      else if ((value = readKeyValue(line, "torrent")) != 0) copyLineValue(source->torrentUrl, SOURCE_URL_MAX, value);
      else if ((value = readKeyValue(line, "list")) != 0) copyLineValue(source->fields.list, SOURCE_FIELD_MAX, value);
      else if ((value = readKeyValue(line, "title")) != 0) copyLineValue(source->fields.title, SOURCE_FIELD_MAX, value);
      else if ((value = readKeyValue(line, "hash")) != 0) copyLineValue(source->fields.hash, SOURCE_FIELD_MAX, value);
      else if ((value = readKeyValue(line, "size")) != 0) copyLineValue(source->fields.size, SOURCE_FIELD_MAX, value);
      else if ((value = readKeyValue(line, "seeds")) != 0) copyLineValue(source->fields.seeds, SOURCE_FIELD_MAX, value);
      else if ((value = readKeyValue(line, "peers")) != 0) copyLineValue(source->fields.peers, SOURCE_FIELD_MAX, value);
      else if ((value = readKeyValue(line, "category")) != 0)
         copyLineValue(source->fields.category, SOURCE_FIELD_MAX, value);
      else if ((value = readKeyValue(line, "adultcategories")) != 0) {
         char range[SOURCE_FIELD_MAX];
         copyLineValue(range, sizeof range, value);

         int at = 0;
         source->adultFrom = readNumber(range, &at);
         if (range[at] == '-') at++;
         source->adultTo = readNumber(range, &at);
      }
      else if ((value = readKeyValue(line, "tracker")) != 0) {
         if (source->trackerCount < SOURCE_TRACKER_MAX)
            copyLineValue(source->trackers[source->trackerCount++], SOURCE_URL_MAX, value);
      } else if ((value = readKeyValue(line, "format")) != 0) {
         char format[16];
         copyLineValue(format, sizeof format, value);

         if (strEq(format, "json")) source->format = SOURCE_FORMAT_JSON;
         else if (!strEq(format, "rss")) {
            logTrace(TAG "sources: %s wants format %s, which this build cannot read\n", source->name, format);
            return -1;
         }
      }

      while (*line && *line != '\n') line++;
      if (*line) line++;
   }

   // a site that cannot be given words to look for is no use as a source: it could only ever show
   // the same handful of newest torrents, whatever was typed
   if (!source->searchUrl[0]) {
      logTrace(TAG "sources: %s has no search address, so it is skipped\n", source->name);
      return -1;
   }

   return 0;
}

// every source file in one folder, added to the list
static int readSourceFolder(const char *directory, TorrentSource *sources, int capacity, int count, int enabled)
{
   VfsDir folder;
   if (openDir(directory, &folder) != 0) return count;

   char fileName[MAX_PATH_LEN];
   VfsEntryType kind;

   while (count < capacity && readDir(&folder, fileName, sizeof fileName, &kind) == 1) {
      if (kind == VFS_ENTRY_DIR) continue;
      if (readSourceFile(&sources[count], directory, fileName) != 0) continue;

      sources[count].enabled = enabled;
      count++;
   }

   closeDir(&folder);
   return count;
}

int loadTorrentSources(const char *directory, TorrentSource *sources, int capacity, const SourceFile *shipped,
                       int shippedCount)
{
   // section: the folders, with the sources the app ships. only on the first launch, so a user who
   // deletes them does not get them back.
   int firstLaunch = !isDir(directory);

   char disabledDirectory[MAX_PATH_LEN];
   joinPath(disabledDirectory, sizeof disabledDirectory, directory, DISABLED_FOLDER);

   if (makeDir(directory) != 0 || makeDir(disabledDirectory) != 0) {
      logError(TAG "sources: %s could not be made\n", directory);
      return 0;
   }

   if (firstLaunch) {
      for (int index = 0; index < shippedCount; index++) {
         char path[MAX_PATH_LEN];
         joinPath(path, sizeof path, directory, shipped[index].fileName);
         writeFile(path, shipped[index].text, getStrLen(shipped[index].text));
      }

      logInfo(TAG "sources: created %d in %s. copy more files in to add sites\n", shippedCount, directory);
   }

   // section: what is in them
   int count = readSourceFolder(directory, sources, capacity, 0, 1);
   int enabledCount = count;
   count = readSourceFolder(disabledDirectory, sources, capacity, count, 0);

   logInfo(TAG "sources: %d in %s, %d turned off\n", enabledCount, directory, count - enabledCount);
   return count;
}

int setSourceEnabled(const char *directory, TorrentSource *source, int enabled)
{
   char disabledDirectory[MAX_PATH_LEN], enabledPath[MAX_PATH_LEN], disabledPath[MAX_PATH_LEN];

   joinPath(disabledDirectory, sizeof disabledDirectory, directory, DISABLED_FOLDER);
   joinPath(enabledPath, sizeof enabledPath, directory, source->fileName);
   joinPath(disabledPath, sizeof disabledPath, disabledDirectory, source->fileName);

   if (source->enabled == enabled) return 0;

   if (renamePath(enabled ? disabledPath : enabledPath, enabled ? enabledPath : disabledPath) != 0) {
      logTrace(TAG "sources: %s could not be moved\n", source->fileName);
      return -1;
   }

   source->enabled = enabled;
   return 0;
}

// anything outside the unreserved set is escaped, which is the safe reading of RFC 3986 for a
// search term: a space, a colon or a bracket in a title would otherwise end up meaning something
static int appendEscaped(char *out, int capacity, int *offset, const char *text)
{
   static const char digits[] = "0123456789ABCDEF";

   for (int index = 0; text[index]; index++) {
      unsigned char character = (unsigned char)text[index];
      int isPlain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                    character == '.' || character == '~';

      if (isPlain) {
         if (*offset >= capacity - 1) return -1;
         out[(*offset)++] = (char)character;
         continue;
      }

      if (*offset >= capacity - 3) return -1;
      out[(*offset)++] = '%';
      out[(*offset)++] = digits[character >> 4];
      out[(*offset)++] = digits[character & 0x0F];
   }

   return 0;
}

// put value wherever {token} appears in pattern, escaped for a URL when the value came from a person
static int fillTemplate(const char *pattern, const char *token, const char *value, int escape, char *out,
                        int capacity)
{
   int offset = 0;
   int patternLength = getStrLen(pattern);
   int tokenLength = getStrLen(token);

   for (int index = 0; index < patternLength;) {
      if (patternLength - index >= tokenLength && findBytes(pattern + index, tokenLength, token, tokenLength) == 0) {
         if (escape) {
            if (appendEscaped(out, capacity, &offset, value) != 0) return -1;
         } else {
            appendStr(out, capacity, &offset, value);
         }

         index += tokenLength;
         continue;
      }

      if (offset >= capacity - 1) return -1;
      out[offset++] = pattern[index++];
   }

   out[offset] = 0;
   return 0;
}

int buildSearchUrl(const TorrentSource *source, const char *query, char *out, int capacity)
{
   if (!source->searchUrl[0] || !query || !query[0]) return -1;

   return fillTemplate(source->searchUrl, "{query}", query, 1, out, capacity);
}

int buildTorrentUrl(const TorrentSource *source, const uint8_t *infoHash, const char *feedLink, char *out,
                    int capacity)
{
   // no address of its own, or no hash to fill it in with: the feed's own link is all there is
   if (!source->torrentUrl[0] || !infoHash) {
      if (!feedLink || !feedLink[0]) return -1;
      strCopy(out, capacity, feedLink);
      return 0;
   }

   char hashText[SHA1_TEXT_LENGTH];
   formatSha1(hashText, infoHash);

   return fillTemplate(source->torrentUrl, "{hash}", hashText, 0, out, capacity);
}
