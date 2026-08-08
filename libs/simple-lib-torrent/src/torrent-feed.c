// torrent-feed - fetch a torrent index's RSS over the tunnel and turn it into a list.
//
// This is not an XML parser. A feed is a run of <item> blocks holding a handful of tags we care
// about, so it looks for those tags between the item's bounds and ignores everything else, which
// keeps a malformed or unexpected feed from being anything worse than a shorter list.

#include "torrent-feed.h"

#include "dbg.h"
#include "format.h"   // formatSize, since a feed words the size and a json answer counts bytes
#include "http.h"
#include "json.h"
#include "string-utilities.h"

#define TAG "[bt] "

#define FEED_MAX 262144   // nyaa's default page is 78 KB across 75 items; this leaves room to grow

static int isHexDigit(char character)
{
   return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F');
}

static int getHexValue(char character)
{
   if (character <= '9') return character - '0';
   return (character | 0x20) - 'a' + 10;
}

// The names HTML gives the characters from 160 to 255, in that order, which is what makes a single
// line of them enough: the position in this list is the character. It covers every accent a European
// title uses, and titles arrive full of them.
static const char *LATIN1_NAMES =
   "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr deg plusmn sup2 sup3 acute "
   "micro para middot cedil sup1 ordm raquo frac14 frac12 frac34 iquest Agrave Aacute Acirc Atilde Auml Aring AElig "
   "Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times "
   "Oslash Ugrave Uacute Ucirc Uuml Yacute THORN szlig agrave aacute acirc atilde auml aring aelig ccedil egrave "
   "eacute ecirc euml igrave iacute icirc iuml eth ntilde ograve oacute ocirc otilde ouml divide oslash ugrave "
   "uacute ucirc uuml yacute thorn yuml";

// which character a name stands for, or 0
static int getNamedCharacter(const char *name, int nameLength)
{
   int character = 160;

   for (const char *entry = LATIN1_NAMES; *entry;) {
      int length = 0;
      while (entry[length] && entry[length] != ' ') length++;

      if (length == nameLength && findBytes(entry, length, name, nameLength) == 0) return character;

      entry += length;
      while (*entry == ' ') entry++;
      character++;
   }

   return 0;
}

// One escape, written out as the character it stands for. Returns how much of the text it took, and
// how many bytes it wrote, which is two for anything past ascii.
static int decodeEntity(const char *text, int length, char *out, int *written)
{
   static const struct { const char *escape; char character; } escapes[] = {
      { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
      { "&apos;", '\'' }, { "&#39;", '\'' }, { "&#039;", '\'' }
   };

   *written = 1;

   for (int index = 0; index < (int)(sizeof escapes / sizeof escapes[0]); index++) {
      int escapeLength = getStrLen(escapes[index].escape);
      if (escapeLength > length) continue;
      if (findBytes(text, escapeLength, escapes[index].escape, escapeLength) != 0) continue;

      out[0] = escapes[index].character;
      return escapeLength;
   }

   // section: a named accent, as two bytes of utf-8
   int nameLength = 1;
   while (nameLength < length && text[nameLength] != ';' && nameLength < 10) nameLength++;

   int character = nameLength < length && text[nameLength] == ';' ? getNamedCharacter(text + 1, nameLength - 1) : 0;
   if (character == 0) {
      out[0] = text[0];
      return 1;
   }

   out[0] = (char)(0xC0 | (character >> 6));
   out[1] = (char)(0x80 | (character & 0x3F));
   *written = 2;
   return nameLength + 1;
}

// Turn the escapes back into characters where they sit. Sites put them in JSON as well as in feeds,
// having escaped their titles once for the web and never undone it. Nothing grows: an escape is
// always longer than what replaces it.
static void decodeEntities(char *text)
{
   int read = 0, written = 0;

   while (text[read]) {
      if (text[read] != '&') { text[written++] = text[read++]; continue; }

      char replacement[2];
      int replacementLength = 0;
      read += decodeEntity(text + read, getStrLen(text + read), replacement, &replacementLength);

      for (int index = 0; index < replacementLength; index++) text[written++] = replacement[index];
   }

   text[written] = 0;
}

// copy what sits between <tag> and </tag>, with the escapes turned back into characters. returns 0
// when the tag was there.
static int copyTagText(const char *item, int length, const char *tag, char *out, int capacity)
{
   // appendStr hands back the length and does not terminate, so the lengths are what gets used
   char opening[40], closing[40];
   int openingLength = 0, closingLength = 0;

   appendStr(opening, sizeof opening, &openingLength, "<");
   appendStr(opening, sizeof opening, &openingLength, tag);
   appendStr(opening, sizeof opening, &openingLength, ">");

   appendStr(closing, sizeof closing, &closingLength, "</");
   appendStr(closing, sizeof closing, &closingLength, tag);
   appendStr(closing, sizeof closing, &closingLength, ">");

   int start = findBytes(item, length, opening, openingLength);
   if (start < 0) return -1;
   start += openingLength;

   int end = findBytes(item + start, length - start, closing, closingLength);
   if (end < 0) return -1;

   int written = 0;
   int index = 0;
   while (index < end && written < capacity - 2) {
      if (item[start + index] != '&') { out[written++] = item[start + index++]; continue; }

      char replacement[2];
      int replacementLength = 0;
      index += decodeEntity(item + start + index, end - index, replacement, &replacementLength);

      for (int part = 0; part < replacementLength; part++) out[written++] = replacement[part];
   }

   out[written] = 0;
   return 0;
}

static int readTagNumber(const char *item, int length, const char *tag)
{
   char text[16];
   if (copyTagText(item, length, tag, text, sizeof text) != 0) return 0;

   int value = 0;
   for (int index = 0; text[index] >= '0' && text[index] <= '9'; index++) value = value * 10 + (text[index] - '0');
   return value;
}

// Forty hex characters become the twenty bytes a client works with
static int readHashText(const char *text, uint8_t *infoHash)
{
   if (getStrLen(text) != INFO_HASH_LENGTH * 2) return 0;

   for (int index = 0; index < INFO_HASH_LENGTH * 2; index++)
      if (!isHexDigit(text[index])) return 0;

   for (int index = 0; index < INFO_HASH_LENGTH; index++)
      infoHash[index] = (uint8_t)((getHexValue(text[index * 2]) << 4) | getHexValue(text[index * 2 + 1]));

   return 1;
}

// sites spell the tag differently, so the ones we have seen are all tried
static int readInfoHash(const char *item, int length, uint8_t *infoHash)
{
   static const char *tags[] = { "infohash", "infoHash", "nyaa:infoHash" };

   char text[64];
   for (int index = 0; index < (int)(sizeof tags / sizeof tags[0]); index++)
      if (copyTagText(item, length, tags[index], text, sizeof text) == 0) return readHashText(text, infoHash);

   return 0;
}

int parseTorrentFeed(const char *feed, int length, TorrentItem *items, int capacity)
{
   int count = 0;
   int offset = 0;

   while (count < capacity) {
      int start = findBytes(feed + offset, length - offset, "<item>", 6);
      if (start < 0) break;
      start += offset + 6;

      int end = findBytes(feed + start, length - start, "</item>", 7);
      if (end < 0) break;

      const char *item = feed + start;
      TorrentItem *torrent = &items[count];
      memSet(torrent, 0, sizeof *torrent);

      // a torrent with no title or no link is not something we could act on, so it is skipped
      if (copyTagText(item, end, "title", torrent->title, sizeof torrent->title) == 0 &&
          copyTagText(item, end, "link", torrent->torrentUrl, sizeof torrent->torrentUrl) == 0) {
         if (copyTagText(item, end, "size", torrent->size, sizeof torrent->size) != 0)
            copyTagText(item, end, "nyaa:size", torrent->size, sizeof torrent->size);
         torrent->seeders = readTagNumber(item, end, "nyaa:seeders");
         torrent->leechers = readTagNumber(item, end, "nyaa:leechers");
         torrent->hasInfoHash = readInfoHash(item, end, torrent->infoHash);
         count++;
      }

      offset = start + end + 7;
   }

   return count;
}

static int isEmptyHash(const uint8_t *infoHash)
{
   for (int index = 0; index < INFO_HASH_LENGTH; index++)
      if (infoHash[index] != 0) return 0;

   return 1;
}

int parseTorrentJson(const TorrentSource *source, const char *text, int length, TorrentItem *items, int capacity)
{
   const SourceFields *fields = &source->fields;

   int start = 0, end = 0;
   if (findJsonArray(text, length, fields->list, &start, &end) != 0) return 0;

   int count = 0;
   int objectStart = 0, objectEnd = 0;

   for (int offset = start; count < capacity;) {
      offset = readJsonObject(text, end, offset, &objectStart, &objectEnd);
      if (offset <= 0) break;

      const char *object = text + objectStart;
      int objectLength = objectEnd - objectStart;

      TorrentItem *torrent = &items[count];
      memSet(torrent, 0, sizeof *torrent);

      // a result with no title or no hash is not something we could act on
      if (getJsonText(object, objectLength, fields->title, torrent->title, sizeof torrent->title) != 0) continue;
      decodeEntities(torrent->title);

      char hashText[64];
      if (getJsonText(object, objectLength, fields->hash, hashText, sizeof hashText) != 0) continue;
      // a hash of nothing but zeroes is what some sites answer with when they found nothing
      torrent->hasInfoHash = readHashText(hashText, torrent->infoHash) && !isEmptyHash(torrent->infoHash);
      if (!torrent->hasInfoHash) continue;

      char size[24];   // what formatSize needs, which is more than a feed's own wording takes
      formatSize((uint64_t)getJsonNumber(object, objectLength, fields->size), size);
      strCopy(torrent->size, sizeof torrent->size, size);
      torrent->seeders = (int)getJsonNumber(object, objectLength, fields->seeds);
      torrent->leechers = (int)getJsonNumber(object, objectLength, fields->peers);
      torrent->category = (int)getJsonNumber(object, objectLength, fields->category);
      count++;
   }

   return count;
}

int loadTorrentResults(const TorrentSource *source, const char *url, TorrentItem *items, int capacity)
{
   static char reply[FEED_MAX];   // far too large for the stack

   int length = 0, status = 0;
   if (getHttp(url, reply, sizeof reply, &length, &status) != 0) {
      logTrace(TAG "feed: %s could not be fetched\n", url);
      return -1;
   }

   if (status != 200) {
      logTrace(TAG "feed: %s answered %d\n", url, status);
      return -1;
   }

   int count = source->format == SOURCE_FORMAT_JSON ? parseTorrentJson(source, reply, length, items, capacity)
                                                    : parseTorrentFeed(reply, length, items, capacity);

   logTrace(TAG "feed: %d bytes from %s, %d torrents\n", length, url, count);
   return count;
}
