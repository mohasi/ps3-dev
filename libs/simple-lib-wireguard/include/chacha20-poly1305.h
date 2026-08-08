#pragma once

// ChaCha20-Poly1305 AEAD (RFC 8439 section 2.8).
//
// The nonce is passed in whole rather than built here. WireGuard's transport nonce is four zero
// bytes followed by the little-endian counter, and that formatting belongs to the protocol layer,
// not to the cipher.
//
// Cookie messages use the extended-nonce form, XChaCha20-Poly1305. It arrives with the cookie
// handling and with the vector that proves it.

#include <stdint.h>

#define AEAD_KEY_LENGTH   32
#define AEAD_NONCE_LENGTH 12
#define AEAD_TAG_LENGTH   16

// writes plainLength + AEAD_TAG_LENGTH bytes to out. out may overlap plain.
void sealChaCha20Poly1305(uint8_t *out, const uint8_t *plain, int plainLength, const uint8_t *aad, int aadLength,
                          const uint8_t nonce[AEAD_NONCE_LENGTH], const uint8_t key[AEAD_KEY_LENGTH]);

// verifies and decrypts sealedLength bytes (ciphertext + tag), writing sealedLength - AEAD_TAG_LENGTH
// bytes to out. returns 1 when the tag is valid, 0 when it is not; out is untouched on failure.
int openChaCha20Poly1305(uint8_t *out, const uint8_t *sealed, int sealedLength, const uint8_t *aad, int aadLength,
                         const uint8_t nonce[AEAD_NONCE_LENGTH], const uint8_t key[AEAD_KEY_LENGTH]);
