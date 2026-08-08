#pragma once

// ChaCha20 stream cipher (RFC 8439).

#include <stdint.h>

#define CHACHA20_KEY_LENGTH   32
#define CHACHA20_NONCE_LENGTH 12
#define CHACHA20_BLOCK_LENGTH 64

// encrypt or decrypt (they are the same operation). out and input may be the same buffer.
void xorChaCha20(uint8_t *out, const uint8_t *input, int length, const uint8_t key[CHACHA20_KEY_LENGTH],
                 const uint8_t nonce[CHACHA20_NONCE_LENGTH], uint32_t blockCounter);

// one raw keystream block, used to make the Poly1305 one-time key.
void getChaCha20Block(uint8_t block[CHACHA20_BLOCK_LENGTH], const uint8_t key[CHACHA20_KEY_LENGTH],
                      const uint8_t nonce[CHACHA20_NONCE_LENGTH], uint32_t blockCounter);
