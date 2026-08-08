#pragma once

// SHA-1 (RFC 3174), which BitTorrent is built on: a torrent names every piece by its SHA-1, the
// info hash that identifies a torrent is the SHA-1 of its description, and a peer id carries one.
// It is not used here for anything that has to resist an attacker, which is just as well.

#include <stdint.h>

#define SHA1_LENGTH 20

typedef struct {
   uint32_t state[5];
   uint64_t totalLength;
   uint8_t  block[64];
   int      blockLength;
} Sha1;

void initSha1(Sha1 *sha1);
void updateSha1(Sha1 *sha1, const void *data, int length);
void finishSha1(Sha1 *sha1, uint8_t *out);

// the whole of one buffer in a single call
void hashSha1(uint8_t *out, const void *data, int length);

#define SHA1_TEXT_LENGTH (SHA1_LENGTH * 2 + 1)

// lower case hex, which is how a hash is written in an address and in a tracker's request
void formatSha1(char *out, const uint8_t *hash);
