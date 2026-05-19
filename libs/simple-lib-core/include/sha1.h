#pragma once

// sha-1, RFC 3174. self-contained, no libc dep (uses string-utilities
// memCopy). used by pkg.h to derive the keystream for debug-type pkgs:
// each block xors the message with the first 16 bytes of sha1(48-byte
// key || 8-byte big-endian counter).

#include <stdint.h>
#include "string-utilities.h"

typedef struct {
    uint32_t h[5];
    uint64_t totalBits;
    uint8_t  buf[64];
    int      bufLen;
} Sha1State;

static inline uint32_t rotateLeft32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void processSha1Block(Sha1State *s, const uint8_t *block)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4+0] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8)  |  (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 80; i++) w[i] = rotateLeft32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | ((~b) & d);     k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                k = 0xCA62C1D6; }
        uint32_t t = rotateLeft32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotateLeft32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void initSha1(Sha1State *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->totalBits = 0;
    s->bufLen = 0;
}

static void updateSha1(Sha1State *s, const uint8_t *data, int len)
{
    s->totalBits += (uint64_t)len * 8;
    while (len > 0) {
        int take = 64 - s->bufLen;
        if (take > len) take = len;
        memCopy(s->buf + s->bufLen, data, take);
        s->bufLen += take;
        data += take;
        len -= take;
        if (s->bufLen == 64) {
            processSha1Block(s, s->buf);
            s->bufLen = 0;
        }
    }
}

static void finalizeSha1(Sha1State *s, uint8_t out[20])
{
    uint64_t bits = s->totalBits;
    s->buf[s->bufLen++] = 0x80;
    if (s->bufLen > 56) {
        while (s->bufLen < 64) s->buf[s->bufLen++] = 0;
        processSha1Block(s, s->buf);
        s->bufLen = 0;
    }
    while (s->bufLen < 56) s->buf[s->bufLen++] = 0;
    for (int i = 7; i >= 0; i--) s->buf[s->bufLen++] = (uint8_t)(bits >> (i * 8));
    processSha1Block(s, s->buf);
    for (int i = 0; i < 5; i++) {
        out[i*4+0] = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

// convenience one-shot.
static void hashSha1(const uint8_t *data, int len, uint8_t out[20])
{
    Sha1State s;
    initSha1(&s);
    updateSha1(&s, data, len);
    finalizeSha1(&s, out);
}
