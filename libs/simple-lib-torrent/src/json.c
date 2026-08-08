// json - reading a search site's answer, which is an array of objects and nothing more elaborate.

#include "json.h"

#include "string-utilities.h"

// a string runs to its closing quote, and a backslash inside it hides whatever follows
static int skipString(const char *text, int length, int offset)
{
   for (offset++; offset < length; offset++) {
      if (text[offset] == '\\') { offset++; continue; }
      if (text[offset] == '"') return offset + 1;
   }

   return length;
}

// past whatever value starts here, however deeply it nests
static int skipValue(const char *text, int length, int offset)
{
   if (offset >= length) return length;

   if (text[offset] == '"') return skipString(text, length, offset);

   if (text[offset] == '{' || text[offset] == '[') {
      int depth = 0;

      while (offset < length) {
         char character = text[offset];

         if (character == '"') { offset = skipString(text, length, offset); continue; }

         if (character == '{' || character == '[') depth++;
         if (character == '}' || character == ']') depth--;

         offset++;
         if (depth == 0) return offset;
      }

      return length;
   }

   // a number, or true, false or null: it runs to the next comma or closing bracket
   while (offset < length && text[offset] != ',' && text[offset] != '}' && text[offset] != ']') offset++;
   return offset;
}

// the member named key, at the top level of this object. returns where its value starts, or -1.
static int findMember(const char *object, int length, const char *key)
{
   int keyLength = getStrLen(key);

   for (int offset = 0; offset < length;) {
      if (object[offset] != '"') { offset++; continue; }

      int nameStart = offset + 1;
      int nameEnd = skipString(object, length, offset) - 1;
      offset = nameEnd + 1;

      while (offset < length && (object[offset] == ' ' || object[offset] == '\n' || object[offset] == '\r' ||
                                 object[offset] == '\t'))
         offset++;

      if (offset >= length || object[offset] != ':') continue;   // a string that was a value, not a name

      offset++;
      while (offset < length && (object[offset] == ' ' || object[offset] == '\n' || object[offset] == '\r' ||
                                 object[offset] == '\t'))
         offset++;

      if (nameEnd - nameStart == keyLength && findBytes(object + nameStart, keyLength, key, keyLength) == 0)
         return offset;

      offset = skipValue(object, length, offset);
   }

   return -1;
}

int findJsonArray(const char *text, int length, const char *key, int *start, int *end)
{
   int offset = 0;

   if (key && key[0]) {
      offset = findMember(text, length, key);
      if (offset < 0) return -1;
   } else {
      while (offset < length && text[offset] != '[') offset++;
   }

   if (offset >= length || text[offset] != '[') return -1;

   *start = offset + 1;
   *end = skipValue(text, length, offset) - 1;
   return *end > *start ? 0 : -1;
}

int readJsonObject(const char *text, int end, int offset, int *objectStart, int *objectEnd)
{
   while (offset < end && text[offset] != '{') offset++;
   if (offset >= end) return 0;

   int past = skipValue(text, end, offset);
   *objectStart = offset + 1;
   *objectEnd = past - 1;

   return past > offset ? past : 0;
}

int getJsonText(const char *object, int length, const char *key, char *out, int capacity)
{
   int offset = findMember(object, length, key);
   if (offset < 0) return -1;

   int written = 0;

   // a string, with the escapes a search result actually carries turned back
   if (object[offset] == '"') {
      for (offset++; offset < length && object[offset] != '"' && written < capacity - 1; offset++) {
         char character = object[offset];

         if (character == '\\' && offset + 1 < length) {
            offset++;
            character = object[offset];
            if (character == 'n' || character == 't' || character == 'r') character = ' ';
            if (character == 'u') { offset += 4; character = '?'; }   // anything not ascii, in one character
         }

         out[written++] = character;
      }

      out[written] = 0;
      return 0;
   }

   // a number or a word, copied as it was written
   while (offset < length && written < capacity - 1 && object[offset] != ',' && object[offset] != '}' &&
          object[offset] != ']')
      out[written++] = object[offset++];

   while (written > 0 && (out[written - 1] == ' ' || out[written - 1] == '\n' || out[written - 1] == '\r')) written--;

   out[written] = 0;
   return 0;
}

int64_t getJsonNumber(const char *object, int length, const char *key)
{
   char text[32];
   if (getJsonText(object, length, key, text, sizeof text) != 0) return 0;

   int index = 0;
   int isNegative = text[0] == '-';
   if (isNegative) index++;

   int64_t value = 0;
   while (text[index] >= '0' && text[index] <= '9') value = value * 10 + (text[index++] - '0');

   return isNegative ? -value : value;
}
