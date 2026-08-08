#include "sha1.h"

#include "string-utilities.h"

// RFC 3174 section 5: the four round constants, one per twenty rounds
#define ROUND_CONSTANT_0 0x5A827999
#define ROUND_CONSTANT_1 0x6ED9EBA1
#define ROUND_CONSTANT_2 0x8F1BBCDC
#define ROUND_CONSTANT_3 0xCA62C1D6

#define BLOCK_LENGTH 64

static uint32_t rotateLeft(uint32_t value, int places)
{
   return (value << places) | (value >> (32 - places));
}

static void compress(uint32_t *state, const uint8_t *block)
{
   uint32_t schedule[80];
   for (int index = 0; index < 16; index++)
      schedule[index] = ((uint32_t)block[index * 4] << 24) | ((uint32_t)block[index * 4 + 1] << 16) |
                        ((uint32_t)block[index * 4 + 2] << 8) | (uint32_t)block[index * 4 + 3];

   for (int index = 16; index < 80; index++)
      schedule[index] = rotateLeft(schedule[index - 3] ^ schedule[index - 8] ^ schedule[index - 14] ^
                                   schedule[index - 16], 1);

   uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

   for (int index = 0; index < 80; index++) {
      uint32_t mixed, constant;
      if (index < 20)      { mixed = (b & c) | (~b & d);            constant = ROUND_CONSTANT_0; }
      else if (index < 40) { mixed = b ^ c ^ d;                     constant = ROUND_CONSTANT_1; }
      else if (index < 60) { mixed = (b & c) | (b & d) | (c & d);   constant = ROUND_CONSTANT_2; }
      else                 { mixed = b ^ c ^ d;                     constant = ROUND_CONSTANT_3; }

      uint32_t next = rotateLeft(a, 5) + mixed + e + constant + schedule[index];
      e = d;
      d = c;
      c = rotateLeft(b, 30);
      b = a;
      a = next;
   }

   state[0] += a;
   state[1] += b;
   state[2] += c;
   state[3] += d;
   state[4] += e;
}

void initSha1(Sha1 *sha1)
{
   sha1->state[0] = 0x67452301;
   sha1->state[1] = 0xEFCDAB89;
   sha1->state[2] = 0x98BADCFE;
   sha1->state[3] = 0x10325476;
   sha1->state[4] = 0xC3D2E1F0;
   sha1->totalLength = 0;
   sha1->blockLength = 0;
}

void updateSha1(Sha1 *sha1, const void *data, int length)
{
   const uint8_t *bytes = (const uint8_t *)data;
   sha1->totalLength += (uint64_t)length;

   // finish the part-filled block first, then take whole blocks straight from the caller's buffer
   while (length > 0) {
      if (sha1->blockLength == 0 && length >= BLOCK_LENGTH) {
         compress(sha1->state, bytes);
         bytes += BLOCK_LENGTH;
         length -= BLOCK_LENGTH;
         continue;
      }

      int room = BLOCK_LENGTH - sha1->blockLength;
      int take = length < room ? length : room;
      memCopy(sha1->block + sha1->blockLength, bytes, take);
      sha1->blockLength += take;
      bytes += take;
      length -= take;

      if (sha1->blockLength == BLOCK_LENGTH) {
         compress(sha1->state, sha1->block);
         sha1->blockLength = 0;
      }
   }
}

void finishSha1(Sha1 *sha1, uint8_t *out)
{
   // a one bit, then zeroes, then the length in bits as a 64 bit number (RFC 3174 section 4)
   uint64_t lengthInBits = sha1->totalLength * 8;

   uint8_t padding = 0x80;
   updateSha1(sha1, &padding, 1);

   uint8_t zero = 0;
   while (sha1->blockLength != BLOCK_LENGTH - 8) updateSha1(sha1, &zero, 1);

   uint8_t lengthBytes[8];
   for (int index = 0; index < 8; index++) lengthBytes[index] = (uint8_t)(lengthInBits >> ((7 - index) * 8));
   updateSha1(sha1, lengthBytes, sizeof lengthBytes);

   for (int index = 0; index < SHA1_LENGTH; index++)
      out[index] = (uint8_t)(sha1->state[index / 4] >> ((3 - (index % 4)) * 8));
}

void hashSha1(uint8_t *out, const void *data, int length)
{
   Sha1 sha1;
   initSha1(&sha1);
   updateSha1(&sha1, data, length);
   finishSha1(&sha1, out);
}

void formatSha1(char *out, const uint8_t *hash)
{
   static const char digits[] = "0123456789abcdef";

   for (int index = 0; index < SHA1_LENGTH; index++) {
      out[index * 2] = digits[hash[index] >> 4];
      out[index * 2 + 1] = digits[hash[index] & 0x0F];
   }
   out[SHA1_LENGTH * 2] = 0;
}
