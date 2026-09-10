#pragma once

// hmac-sha1, RFC 2104. self-contained, built on sha1.h, no libc dep.
//
// Added for the connectivity checks two ends of a streaming connection send each other: every one
// carries a hash of itself keyed on the other end's password, which is how each side knows the
// message came from who it claims.

#include <stdint.h>
#include "sha1.h"
#include "string-utilities.h"

#define HMAC_SHA1_BLOCK 64

typedef struct {
   Sha1State inner;
   uint8_t   outerKey[HMAC_SHA1_BLOCK];
} HmacSha1State;

static inline void initHmacSha1(HmacSha1State *state, const uint8_t *key, int keyLength)
{
   uint8_t innerKey[HMAC_SHA1_BLOCK];
   uint8_t shortened[20];

   // a key longer than the block is replaced by its own hash, per RFC 2104
   if (keyLength > HMAC_SHA1_BLOCK) {
      hashSha1(key, keyLength, shortened);
      key = shortened;
      keyLength = 20;
   }

   memSet(innerKey, 0, sizeof innerKey);
   memSet(state->outerKey, 0, sizeof state->outerKey);
   memCopy(innerKey, key, keyLength);
   memCopy(state->outerKey, key, keyLength);
   for (int at = 0; at < HMAC_SHA1_BLOCK; at++) {
      innerKey[at] ^= 0x36;
      state->outerKey[at] ^= 0x5c;
   }

   initSha1(&state->inner);
   updateSha1(&state->inner, innerKey, HMAC_SHA1_BLOCK);
}

static inline void updateHmacSha1(HmacSha1State *state, const uint8_t *data, int length)
{
   updateSha1(&state->inner, data, length);
}

static inline void finalizeHmacSha1(HmacSha1State *state, uint8_t out[20])
{
   uint8_t innerHash[20];
   finalizeSha1(&state->inner, innerHash);

   Sha1State outer;
   initSha1(&outer);
   updateSha1(&outer, state->outerKey, HMAC_SHA1_BLOCK);
   updateSha1(&outer, innerHash, 20);
   finalizeSha1(&outer, out);
}

static inline void hmacSha1(const uint8_t *key, int keyLength, const uint8_t *data, int length, uint8_t out[20])
{
   HmacSha1State state;
   initHmacSha1(&state, key, keyLength);
   updateHmacSha1(&state, data, length);
   finalizeHmacSha1(&state, out);
}
