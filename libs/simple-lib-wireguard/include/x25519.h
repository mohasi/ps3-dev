#pragma once

// X25519 Diffie-Hellman on Curve25519 (RFC 7748).

#include <stdint.h>

#define X25519_KEY_LENGTH 32

void getX25519PublicKey(uint8_t publicKey[X25519_KEY_LENGTH], const uint8_t privateKey[X25519_KEY_LENGTH]);

// returns 0 when the result is all zero, which means the peer sent a low-order point and the
// shared secret must not be used. 1 otherwise.
int computeX25519Shared(uint8_t shared[X25519_KEY_LENGTH], const uint8_t privateKey[X25519_KEY_LENGTH],
                        const uint8_t peerPublicKey[X25519_KEY_LENGTH]);
