#include "poly1305.h"

#include "string-utilities.h"   // memSet, memCopy
#include "wg-bytes.h"

// The accumulator is held in five 26-bit limbs so that every partial product fits in 64 bits
// without needing a 128-bit type, which this compiler (GCC 4.1.1) does not have. A 64-bit limb
// layout would be faster on the PPU and is a candidate for the later optimisation pass.

#define LIMB_MASK 0x3FFFFFFu

static void processPoly1305Block(Poly1305State *state, const uint8_t *block, uint32_t highBit)
{
   const uint32_t r0 = state->r[0], r1 = state->r[1], r2 = state->r[2], r3 = state->r[3], r4 = state->r[4];
   const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

   // add the message block, read as a 130-bit little-endian number with the high bit set
   uint32_t h0 = state->accumulator[0] + (load32le(block) & LIMB_MASK);
   uint32_t h1 = state->accumulator[1] + ((load32le(block + 3) >> 2) & LIMB_MASK);
   uint32_t h2 = state->accumulator[2] + ((load32le(block + 6) >> 4) & LIMB_MASK);
   uint32_t h3 = state->accumulator[3] + ((load32le(block + 9) >> 6) & LIMB_MASK);
   uint32_t h4 = state->accumulator[4] + ((load32le(block + 12) >> 8) | highBit);

   // multiply by r modulo 2^130 - 5; the s values fold the overflow limbs back in
   uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
   uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
   uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
   uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
   uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

   // propagate carries back down the limbs
   uint32_t carry = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & LIMB_MASK;
   d1 += carry; carry = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & LIMB_MASK;
   d2 += carry; carry = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & LIMB_MASK;
   d3 += carry; carry = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & LIMB_MASK;
   d4 += carry; carry = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & LIMB_MASK;
   h0 += carry * 5; carry = h0 >> 26; h0 &= LIMB_MASK;
   h1 += carry;

   state->accumulator[0] = h0;
   state->accumulator[1] = h1;
   state->accumulator[2] = h2;
   state->accumulator[3] = h3;
   state->accumulator[4] = h4;
}

void initPoly1305(Poly1305State *state, const uint8_t key[POLY1305_KEY_LENGTH])
{
   memSet(state, 0, sizeof *state);

   // clamp r, RFC 8439 section 2.5.1: certain bits of the first key half are cleared
   state->r[0] = (load32le(key)) & 0x3FFFFFF;
   state->r[1] = (load32le(key + 3) >> 2) & 0x3FFFF03;
   state->r[2] = (load32le(key + 6) >> 4) & 0x3FFC0FF;
   state->r[3] = (load32le(key + 9) >> 6) & 0x3F03FFF;
   state->r[4] = (load32le(key + 12) >> 8) & 0x00FFFFF;

   for (int index = 0; index < 4; index++) state->pad[index] = load32le(key + 16 + index * 4);
}

void updatePoly1305(Poly1305State *state, const void *data, int length)
{
   const uint8_t *input = (const uint8_t *)data;

   // finish any part-filled block first
   if (state->blockUsed > 0) {
      int room = 16 - state->blockUsed;
      int take = length < room ? length : room;
      memCopy(state->block + state->blockUsed, input, take);
      state->blockUsed += take;
      input += take;
      length -= take;
      if (state->blockUsed < 16) return;
      processPoly1305Block(state, state->block, 1u << 24);
      state->blockUsed = 0;
   }

   while (length >= 16) {
      processPoly1305Block(state, input, 1u << 24);
      input += 16;
      length -= 16;
   }

   if (length > 0) {
      memCopy(state->block, input, length);
      state->blockUsed = length;
   }
}

void finishPoly1305(Poly1305State *state, uint8_t tag[POLY1305_TAG_LENGTH])
{
   // a short final block is terminated by a 1 byte instead of the implicit high bit
   if (state->blockUsed > 0) {
      state->block[state->blockUsed++] = 1;
      memSet(state->block + state->blockUsed, 0, 16 - state->blockUsed);
      processPoly1305Block(state, state->block, 0);
   }

   uint32_t h0 = state->accumulator[0], h1 = state->accumulator[1], h2 = state->accumulator[2];
   uint32_t h3 = state->accumulator[3], h4 = state->accumulator[4];

   uint32_t carry = h1 >> 26; h1 &= LIMB_MASK;
   h2 += carry; carry = h2 >> 26; h2 &= LIMB_MASK;
   h3 += carry; carry = h3 >> 26; h3 &= LIMB_MASK;
   h4 += carry; carry = h4 >> 26; h4 &= LIMB_MASK;
   h0 += carry * 5; carry = h0 >> 26; h0 &= LIMB_MASK;
   h1 += carry;

   // compute accumulator + 5; if that did not borrow, the accumulator was >= 2^130 - 5 and the
   // reduced value is the one to keep. selected without branching.
   uint32_t g0 = h0 + 5; carry = g0 >> 26; g0 &= LIMB_MASK;
   uint32_t g1 = h1 + carry; carry = g1 >> 26; g1 &= LIMB_MASK;
   uint32_t g2 = h2 + carry; carry = g2 >> 26; g2 &= LIMB_MASK;
   uint32_t g3 = h3 + carry; carry = g3 >> 26; g3 &= LIMB_MASK;
   uint32_t g4 = h4 + carry - (1u << 26);

   uint32_t selectG = (g4 >> 31) - 1;
   uint32_t selectH = ~selectG;
   h0 = (h0 & selectH) | (g0 & selectG);
   h1 = (h1 & selectH) | (g1 & selectG);
   h2 = (h2 & selectH) | (g2 & selectG);
   h3 = (h3 & selectH) | (g3 & selectG);
   h4 = (h4 & selectH) | (g4 & selectG);

   // repack the limbs into four 32-bit words, then add the second key half
   uint32_t word0 = (h0 | (h1 << 26));
   uint32_t word1 = ((h1 >> 6) | (h2 << 20));
   uint32_t word2 = ((h2 >> 12) | (h3 << 14));
   uint32_t word3 = ((h3 >> 18) | (h4 << 8));

   uint64_t sum = (uint64_t)word0 + state->pad[0]; word0 = (uint32_t)sum;
   sum = (uint64_t)word1 + state->pad[1] + (sum >> 32); word1 = (uint32_t)sum;
   sum = (uint64_t)word2 + state->pad[2] + (sum >> 32); word2 = (uint32_t)sum;
   sum = (uint64_t)word3 + state->pad[3] + (sum >> 32); word3 = (uint32_t)sum;

   store32le(tag, word0);
   store32le(tag + 4, word1);
   store32le(tag + 8, word2);
   store32le(tag + 12, word3);

   memSet(state, 0, sizeof *state);
}

void authPoly1305(uint8_t tag[POLY1305_TAG_LENGTH], const uint8_t key[POLY1305_KEY_LENGTH], const void *data,
                  int length)
{
   Poly1305State state;
   initPoly1305(&state, key);
   updatePoly1305(&state, data, length);
   finishPoly1305(&state, tag);
}
