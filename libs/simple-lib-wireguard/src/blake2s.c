#include "blake2s.h"

#include "string-utilities.h"   // memCopy, memSet: the library never calls libc
#include "wg-bytes.h"

// initialisation vector, RFC 7693 section 2.6 (the SHA-256 IV).
static const uint32_t blake2sIv[8] = {
   0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
};

// message word permutations, RFC 7693 section 2.7.
static const uint8_t blake2sSigma[10][16] = {
   {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
   { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
   { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
   {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
   {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
   {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
   { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
   { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
   {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
   { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 }
};

#define MIX(a, b, c, d, x, y)                 \
   do {                                       \
      a = a + b + (x);                        \
      d = rotateRight32(d ^ a, 16);           \
      c = c + d;                              \
      b = rotateRight32(b ^ c, 12);           \
      a = a + b + (y);                        \
      d = rotateRight32(d ^ a, 8);            \
      c = c + d;                              \
      b = rotateRight32(b ^ c, 7);            \
   } while (0)

static void compressBlake2s(Blake2sState *state, const uint8_t *block, int isLastBlock)
{
   uint32_t message[16];
   for (int index = 0; index < 16; index++) message[index] = load32le(block + index * 4);

   uint32_t work[16];
   for (int index = 0; index < 8; index++) work[index] = state->chain[index];
   for (int index = 0; index < 8; index++) work[8 + index] = blake2sIv[index];

   work[12] ^= (uint32_t)state->byteCounter;
   work[13] ^= (uint32_t)(state->byteCounter >> 32);
   if (isLastBlock) work[14] = ~work[14];

   for (int round = 0; round < 10; round++) {
      const uint8_t *order = blake2sSigma[round];
      MIX(work[0], work[4], work[ 8], work[12], message[order[ 0]], message[order[ 1]]);
      MIX(work[1], work[5], work[ 9], work[13], message[order[ 2]], message[order[ 3]]);
      MIX(work[2], work[6], work[10], work[14], message[order[ 4]], message[order[ 5]]);
      MIX(work[3], work[7], work[11], work[15], message[order[ 6]], message[order[ 7]]);
      MIX(work[0], work[5], work[10], work[15], message[order[ 8]], message[order[ 9]]);
      MIX(work[1], work[6], work[11], work[12], message[order[10]], message[order[11]]);
      MIX(work[2], work[7], work[ 8], work[13], message[order[12]], message[order[13]]);
      MIX(work[3], work[4], work[ 9], work[14], message[order[14]], message[order[15]]);
   }

   for (int index = 0; index < 8; index++) state->chain[index] ^= work[index] ^ work[8 + index];
}

void initBlake2s(Blake2sState *state, int hashLength)
{
   memSet(state, 0, sizeof *state);
   for (int index = 0; index < 8; index++) state->chain[index] = blake2sIv[index];

   // parameter block, RFC 7693 section 2.5: digest length, key length, fanout 1, depth 1.
   state->chain[0] ^= 0x01010000u ^ (uint32_t)hashLength;
   state->hashLength = hashLength;
}

void initBlake2sKeyed(Blake2sState *state, int hashLength, const uint8_t *key, int keyLength)
{
   if (!key || keyLength <= 0) { initBlake2s(state, hashLength); return; }

   memSet(state, 0, sizeof *state);
   for (int index = 0; index < 8; index++) state->chain[index] = blake2sIv[index];
   state->chain[0] ^= 0x01010000u ^ ((uint32_t)keyLength << 8) ^ (uint32_t)hashLength;
   state->hashLength = hashLength;

   // the key becomes a full first block, zero padded, compressed like any other block
   memCopy(state->block, key, keyLength);
   state->blockUsed = BLAKE2S_BLOCK_LENGTH;
}

void updateBlake2s(Blake2sState *state, const void *data, int length)
{
   const uint8_t *input = (const uint8_t *)data;

   while (length > 0) {
      // a full buffer is only compressed once more input is known to follow, because the final
      // block has to be compressed with the last-block flag set.
      if (state->blockUsed == BLAKE2S_BLOCK_LENGTH) {
         state->byteCounter += BLAKE2S_BLOCK_LENGTH;
         compressBlake2s(state, state->block, 0);
         state->blockUsed = 0;
      }

      int room = BLAKE2S_BLOCK_LENGTH - state->blockUsed;
      int take = length < room ? length : room;
      memCopy(state->block + state->blockUsed, input, take);
      state->blockUsed += take;
      input += take;
      length -= take;
   }
}

void finishBlake2s(Blake2sState *state, uint8_t *hash)
{
   state->byteCounter += (uint64_t)state->blockUsed;
   memSet(state->block + state->blockUsed, 0, BLAKE2S_BLOCK_LENGTH - state->blockUsed);
   compressBlake2s(state, state->block, 1);

   uint8_t full[BLAKE2S_HASH_LENGTH];
   for (int index = 0; index < 8; index++) store32le(full + index * 4, state->chain[index]);
   memCopy(hash, full, state->hashLength);
   memSet(state, 0, sizeof *state);
}

void hashBlake2s(uint8_t *hash, int hashLength, const void *data, int length)
{
   Blake2sState state;
   initBlake2s(&state, hashLength);
   updateBlake2s(&state, data, length);
   finishBlake2s(&state, hash);
}

void macBlake2s(uint8_t *mac, int macLength, const uint8_t *key, int keyLength, const void *data, int length)
{
   Blake2sState state;
   initBlake2sKeyed(&state, macLength, key, keyLength);
   updateBlake2s(&state, data, length);
   finishBlake2s(&state, mac);
}

void hmacBlake2s(uint8_t mac[BLAKE2S_HASH_LENGTH], const uint8_t *key, int keyLength, const void *data, int length)
{
   // RFC 2104 construction. a key longer than one block is hashed down to 32 bytes first.
   uint8_t blockKey[BLAKE2S_BLOCK_LENGTH];
   memSet(blockKey, 0, sizeof blockKey);
   if (keyLength > BLAKE2S_BLOCK_LENGTH) hashBlake2s(blockKey, BLAKE2S_HASH_LENGTH, key, keyLength);
   else if (keyLength > 0) memCopy(blockKey, key, keyLength);

   uint8_t pad[BLAKE2S_BLOCK_LENGTH];
   Blake2sState state;

   for (int index = 0; index < BLAKE2S_BLOCK_LENGTH; index++) pad[index] = (uint8_t)(blockKey[index] ^ 0x36);
   initBlake2s(&state, BLAKE2S_HASH_LENGTH);
   updateBlake2s(&state, pad, BLAKE2S_BLOCK_LENGTH);
   updateBlake2s(&state, data, length);
   finishBlake2s(&state, mac);

   for (int index = 0; index < BLAKE2S_BLOCK_LENGTH; index++) pad[index] = (uint8_t)(blockKey[index] ^ 0x5C);
   initBlake2s(&state, BLAKE2S_HASH_LENGTH);
   updateBlake2s(&state, pad, BLAKE2S_BLOCK_LENGTH);
   updateBlake2s(&state, mac, BLAKE2S_HASH_LENGTH);
   finishBlake2s(&state, mac);

   memSet(blockKey, 0, sizeof blockKey);
   memSet(pad, 0, sizeof pad);
}
