#include "chacha20.h"

#include "wg-bytes.h"

// "expand 32-byte k", RFC 8439 section 2.3.
static const uint32_t chachaConstants[4] = { 0x61707865, 0x3320646E, 0x79622D32, 0x6B206574 };

#define QUARTER_ROUND(a, b, c, d)             \
   do {                                       \
      a += b; d = rotateLeft32(d ^ a, 16);    \
      c += d; b = rotateLeft32(b ^ c, 12);    \
      a += b; d = rotateLeft32(d ^ a, 8);     \
      c += d; b = rotateLeft32(b ^ c, 7);     \
   } while (0)

static void runChaChaRounds(uint32_t state[16])
{
   for (int doubleRound = 0; doubleRound < 10; doubleRound++) {
      QUARTER_ROUND(state[0], state[4], state[ 8], state[12]);
      QUARTER_ROUND(state[1], state[5], state[ 9], state[13]);
      QUARTER_ROUND(state[2], state[6], state[10], state[14]);
      QUARTER_ROUND(state[3], state[7], state[11], state[15]);
      QUARTER_ROUND(state[0], state[5], state[10], state[15]);
      QUARTER_ROUND(state[1], state[6], state[11], state[12]);
      QUARTER_ROUND(state[2], state[7], state[ 8], state[13]);
      QUARTER_ROUND(state[3], state[4], state[ 9], state[14]);
   }
}

static void buildState(uint32_t state[16], const uint8_t key[CHACHA20_KEY_LENGTH], uint32_t blockCounter,
                       const uint8_t nonce[CHACHA20_NONCE_LENGTH])
{
   for (int index = 0; index < 4; index++) state[index] = chachaConstants[index];
   for (int index = 0; index < 8; index++) state[4 + index] = load32le(key + index * 4);
   state[12] = blockCounter;
   state[13] = load32le(nonce);
   state[14] = load32le(nonce + 4);
   state[15] = load32le(nonce + 8);
}

void getChaCha20Block(uint8_t block[CHACHA20_BLOCK_LENGTH], const uint8_t key[CHACHA20_KEY_LENGTH],
                      const uint8_t nonce[CHACHA20_NONCE_LENGTH], uint32_t blockCounter)
{
   uint32_t state[16], working[16];
   buildState(state, key, blockCounter, nonce);
   for (int index = 0; index < 16; index++) working[index] = state[index];

   runChaChaRounds(working);
   for (int index = 0; index < 16; index++) store32le(block + index * 4, working[index] + state[index]);
}

void xorChaCha20(uint8_t *out, const uint8_t *input, int length, const uint8_t key[CHACHA20_KEY_LENGTH],
                 const uint8_t nonce[CHACHA20_NONCE_LENGTH], uint32_t blockCounter)
{
   uint8_t keyStream[CHACHA20_BLOCK_LENGTH];
   int done = 0;

   while (done < length) {
      getChaCha20Block(keyStream, key, nonce, blockCounter);
      blockCounter++;

      int remaining = length - done;
      int take = remaining < CHACHA20_BLOCK_LENGTH ? remaining : CHACHA20_BLOCK_LENGTH;
      for (int index = 0; index < take; index++) out[done + index] = (uint8_t)(input[done + index] ^ keyStream[index]);
      done += take;
   }
}
