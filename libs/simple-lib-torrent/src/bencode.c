#include "bencode.h"

#include "string-utilities.h"

#define DEPTH_MAX 16   // a torrent nests three or four deep; anything past this is not one

static int readValue(const uint8_t *document, int length, int offset, BencodeValue *out, int depth);

// an integer runs from after its marker to the 'e', and may be negative
static int readInteger(const uint8_t *document, int length, int offset, BencodeValue *out)
{
   int cursor = offset + 1;
   if (cursor < length && document[cursor] == '-') cursor++;

   int digits = 0;
   while (cursor < length && document[cursor] >= '0' && document[cursor] <= '9') { cursor++; digits++; }
   if (digits == 0 || cursor >= length || document[cursor] != 'e') return -1;

   out->kind = BENCODE_INTEGER;
   out->start = offset;
   out->end = cursor + 1;
   out->innerStart = offset + 1;
   out->innerEnd = cursor;
   return out->end;
}

// a string is its length in decimal, a colon, then exactly that many bytes
static int readString(const uint8_t *document, int length, int offset, BencodeValue *out)
{
   int cursor = offset;
   int textLength = 0;
   int digits = 0;

   while (cursor < length && document[cursor] >= '0' && document[cursor] <= '9') {
      if (textLength > (1 << 28)) return -1;   // longer than anything we would hold
      textLength = textLength * 10 + (document[cursor] - '0');
      cursor++;
      digits++;
   }

   if (digits == 0 || cursor >= length || document[cursor] != ':') return -1;
   cursor++;
   if (textLength > length - cursor) return -1;

   out->kind = BENCODE_STRING;
   out->start = offset;
   out->end = cursor + textLength;
   out->innerStart = cursor;
   out->innerEnd = cursor + textLength;
   return out->end;
}

// a list and a dictionary are read the same way: values until the 'e' that closes them
static int readContainer(const uint8_t *document, int length, int offset, BencodeValue *out, int depth,
                         BencodeKind kind)
{
   int cursor = offset + 1;
   while (cursor < length && document[cursor] != 'e') {
      BencodeValue member;
      cursor = readValue(document, length, cursor, &member, depth + 1);
      if (cursor < 0) return -1;
   }

   if (cursor >= length) return -1;

   out->kind = kind;
   out->start = offset;
   out->end = cursor + 1;
   out->innerStart = offset + 1;
   out->innerEnd = cursor;
   return out->end;
}

static int readValue(const uint8_t *document, int length, int offset, BencodeValue *out, int depth)
{
   if (offset < 0 || offset >= length || depth > DEPTH_MAX) return -1;

   if (document[offset] == 'i') return readInteger(document, length, offset, out);
   if (document[offset] == 'l') return readContainer(document, length, offset, out, depth, BENCODE_LIST);
   if (document[offset] == 'd') return readContainer(document, length, offset, out, depth, BENCODE_DICTIONARY);
   if (document[offset] >= '0' && document[offset] <= '9') return readString(document, length, offset, out);

   return -1;
}

int readBencode(const uint8_t *document, int length, int offset, BencodeValue *out)
{
   return readValue(document, length, offset, out, 0);
}

int readBencodeItem(const uint8_t *document, int length, const BencodeValue *container, int offset,
                    BencodeValue *out)
{
   if (offset >= container->innerEnd) return 0;
   return readValue(document, length, offset, out, 0);
}

int findBencodeMember(const uint8_t *document, int length, const BencodeValue *dictionary, const char *name,
                      BencodeValue *out)
{
   if (dictionary->kind != BENCODE_DICTIONARY) return -1;

   int nameLength = getStrLen(name);
   int cursor = dictionary->innerStart;

   while (cursor < dictionary->innerEnd) {
      BencodeValue key;
      cursor = readValue(document, length, cursor, &key, 0);
      if (cursor < 0 || key.kind != BENCODE_STRING) return -1;

      BencodeValue value;
      int next = readValue(document, length, cursor, &value, 0);
      if (next < 0) return -1;

      if (key.innerEnd - key.innerStart == nameLength &&
          findBytes((const char *)document + key.innerStart, nameLength, name, nameLength) == 0) {
         *out = value;
         return 0;
      }

      cursor = next;
   }

   return -1;
}

int64_t getBencodeInteger(const uint8_t *document, const BencodeValue *value)
{
   if (value->kind != BENCODE_INTEGER) return 0;

   int cursor = value->innerStart;
   int isNegative = document[cursor] == '-';
   if (isNegative) cursor++;

   int64_t result = 0;
   while (cursor < value->innerEnd) result = result * 10 + (document[cursor++] - '0');
   return isNegative ? -result : result;
}

int getBencodeString(const uint8_t *document, const BencodeValue *value, const uint8_t **text)
{
   if (value->kind != BENCODE_STRING) return -1;

   *text = document + value->innerStart;
   return value->innerEnd - value->innerStart;
}

int copyBencodeString(const uint8_t *document, const BencodeValue *value, char *out, int capacity)
{
   const uint8_t *text = 0;
   int textLength = getBencodeString(document, value, &text);
   if (textLength < 0) return -1;

   int take = textLength < capacity - 1 ? textLength : capacity - 1;
   memCopy(out, text, take);
   out[take] = 0;
   return 0;
}
