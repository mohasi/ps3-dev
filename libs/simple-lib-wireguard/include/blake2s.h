#pragma once

// BLAKE2s (RFC 7693). WireGuard uses it three ways: a 32-byte hash for the handshake chaining
// values, a 16-byte keyed MAC for mac1 and mac2, and as the hash inside the key derivation.
//
// HMAC over a hash that already has a keyed mode is redundant, but Noise specifies HKDF and
// WireGuard follows it, so the construction is needed here too.

#include <stdint.h>

#define BLAKE2S_BLOCK_LENGTH 64
#define BLAKE2S_HASH_LENGTH  32

typedef struct {
   uint32_t chain[8];
   uint64_t byteCounter;                    // total bytes fed to the compression function so far
   uint8_t  block[BLAKE2S_BLOCK_LENGTH];
   int      blockUsed;
   int      hashLength;
} Blake2sState;

void initBlake2s(Blake2sState *state, int hashLength);
void initBlake2sKeyed(Blake2sState *state, int hashLength, const uint8_t *key, int keyLength);
void updateBlake2s(Blake2sState *state, const void *data, int length);
void finishBlake2s(Blake2sState *state, uint8_t *hash);

void hashBlake2s(uint8_t *hash, int hashLength, const void *data, int length);
void macBlake2s(uint8_t *mac, int macLength, const uint8_t *key, int keyLength, const void *data, int length);

void hmacBlake2s(uint8_t mac[BLAKE2S_HASH_LENGTH], const uint8_t *key, int keyLength, const void *data, int length);
