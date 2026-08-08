#pragma once

// The only place in the library that converts between bytes and integers.
//
// Every WireGuard primitive is specified on octets: ChaCha20 and Poly1305 (RFC 8439), BLAKE2s
// (RFC 7693) and X25519 (RFC 7748) all define their words as little-endian byte sequences. Going
// through these helpers means no code ever casts a byte buffer to a wider integer, so the library
// produces identical results on the big-endian PPU and on any little-endian host.

#include <stdint.h>

static inline uint32_t load32le(const uint8_t *bytes)
{
   return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static inline void store32le(uint8_t *out, uint32_t value)
{
   out[0] = (uint8_t)(value);
   out[1] = (uint8_t)(value >> 8);
   out[2] = (uint8_t)(value >> 16);
   out[3] = (uint8_t)(value >> 24);
}

static inline uint64_t load64le(const uint8_t *bytes)
{
   return (uint64_t)load32le(bytes) | ((uint64_t)load32le(bytes + 4) << 32);
}

static inline void store64le(uint8_t *out, uint64_t value)
{
   store32le(out, (uint32_t)value);
   store32le(out + 4, (uint32_t)(value >> 32));
}

// IP and DNS put their numbers the other way round, biggest byte first.
static inline uint16_t load16be(const uint8_t *bytes)
{
   return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static inline uint32_t load32be(const uint8_t *bytes)
{
   return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static inline void store16be(uint8_t *out, uint16_t value)
{
   out[0] = (uint8_t)(value >> 8);
   out[1] = (uint8_t)value;
}

static inline void store32be(uint8_t *out, uint32_t value)
{
   out[0] = (uint8_t)(value >> 24);
   out[1] = (uint8_t)(value >> 16);
   out[2] = (uint8_t)(value >> 8);
   out[3] = (uint8_t)value;
}

// WireGuard's AEAD nonce: four zero bytes then the counter, little-endian. Used by both the
// handshake and the data path, so it lives here with the rest of the byte layout.
static inline void storeWgNonce(uint8_t nonce[12], uint64_t counter)
{
   nonce[0] = nonce[1] = nonce[2] = nonce[3] = 0;
   store64le(nonce + 4, counter);
}

static inline uint32_t rotateLeft32(uint32_t value, int bits)
{
   return (value << bits) | (value >> (32 - bits));
}

static inline uint32_t rotateRight32(uint32_t value, int bits)
{
   return (value >> bits) | (value << (32 - bits));
}

// compare in constant time so a mismatch never reveals how many bytes matched. 1 when equal.
static inline int bytesEqual(const uint8_t *a, const uint8_t *b, int length)
{
   uint8_t difference = 0;
   for (int index = 0; index < length; index++) difference |= (uint8_t)(a[index] ^ b[index]);
   return difference == 0;
}
