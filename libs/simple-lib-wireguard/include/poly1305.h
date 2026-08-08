#pragma once

// Poly1305 one-time authenticator (RFC 8439 section 2.5).

#include <stdint.h>

#define POLY1305_KEY_LENGTH 32
#define POLY1305_TAG_LENGTH 16

typedef struct {
   uint32_t r[5];          // the clamped key, in five 26-bit limbs
   uint32_t accumulator[5];
   uint32_t pad[4];        // the second half of the key, added at the end
   uint8_t  block[16];
   int      blockUsed;
} Poly1305State;

void initPoly1305(Poly1305State *state, const uint8_t key[POLY1305_KEY_LENGTH]);
void updatePoly1305(Poly1305State *state, const void *data, int length);
void finishPoly1305(Poly1305State *state, uint8_t tag[POLY1305_TAG_LENGTH]);

void authPoly1305(uint8_t tag[POLY1305_TAG_LENGTH], const uint8_t key[POLY1305_KEY_LENGTH], const void *data,
                  int length);
