// magnet - reading a magnet link into the little it actually says.

#include "magnet.h"

#include "dbg.h"
#include "string-utilities.h"

#define TAG "[bt] "

#define HASH_PREFIX "urn:btih:"
#define HEX_LENGTH    (SHA1_LENGTH * 2)
#define BASE32_LENGTH 32

static int readHexHash(const char *text, uint8_t *infoHash)
{
   for (int index = 0; index < HEX_LENGTH; index++)
      if (hexDigit(text[index]) < 0) return -1;

   for (int index = 0; index < SHA1_LENGTH; index++)
      infoHash[index] = (uint8_t)((hexDigit(text[index * 2]) << 4) | hexDigit(text[index * 2 + 1]));

   return 0;
}

// base32 as RFC 4648: five bits a character, so thirty two characters carry the twenty bytes
static int readBase32Hash(const char *text, uint8_t *infoHash)
{
   uint32_t bits = 0;
   int held = 0;
   int written = 0;

   for (int index = 0; index < BASE32_LENGTH; index++) {
      char character = toUpperChar(text[index]);
      int value;

      if (character >= 'A' && character <= 'Z') value = character - 'A';
      else if (character >= '2' && character <= '7') value = character - '2' + 26;
      else return -1;

      bits = (bits << 5) | (uint32_t)value;
      held += 5;

      if (held < 8) continue;

      held -= 8;
      infoHash[written++] = (uint8_t)(bits >> held);
   }

   return written == SHA1_LENGTH ? 0 : -1;
}

// one "key=value" out of the query, with the value decoded
static int readParameter(const char *text, int length, const char *key, char *value, int capacity)
{
   int keyLength = getStrLen(key);
   if (length <= keyLength || findBytes(text, keyLength, key, keyLength) != 0) return -1;

   char raw[TRACKER_URL_MAX * 2];
   int take = length - keyLength;
   if (take >= (int)sizeof raw) return -1;

   memCopy(raw, text + keyLength, take);
   raw[take] = 0;

   return urlDecode(raw, value, capacity) >= 0 ? 0 : -1;
}

int isMagnetLink(const char *text)
{
   return startsWith(text, "magnet:?");
}

int readMagnetLink(MagnetLink *magnet, const char *uri)
{
   memSet(magnet, 0, sizeof *magnet);
   if (!isMagnetLink(uri)) return -1;

   int haveHash = 0;

   for (const char *part = uri + 8; *part;) {
      int length = 0;
      while (part[length] && part[length] != '&') length++;

      char value[TRACKER_URL_MAX];

      // section: the hash, which is the one part a magnet cannot do without
      if (readParameter(part, length, "xt=", value, sizeof value) == 0 && startsWith(value, HASH_PREFIX)) {
         const char *hashText = value + getStrLen(HASH_PREFIX);
         int hashLength = getStrLen(hashText);

         if (hashLength == HEX_LENGTH) haveHash = readHexHash(hashText, magnet->infoHash) == 0;
         else if (hashLength == BASE32_LENGTH) haveHash = readBase32Hash(hashText, magnet->infoHash) == 0;
      }

      // section: the parts that are nice to have
      if (readParameter(part, length, "dn=", value, sizeof value) == 0)
         strCopy(magnet->name, sizeof magnet->name, value);

      if (magnet->trackerCount < TRACKER_MAX && readParameter(part, length, "tr=", value, sizeof value) == 0)
         strCopy(magnet->trackers[magnet->trackerCount++], TRACKER_URL_MAX, value);

      part += length;
      if (*part) part++;
   }

   if (!haveHash) return -1;   // the caller says so: a refusal is a normal answer here

   if (!magnet->name[0]) strCopy(magnet->name, sizeof magnet->name, "unnamed");
   return 0;
}
