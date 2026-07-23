//
// gdrive-crypto.c - console-bound credential obfuscation (see gdrive-crypto.h).
//
// Layout of the encrypted blob (before hex encoding):
//   [16-byte random nonce][ciphertext bytes]
// The keystream is SHA-256(consoleKey || nonce || blockCounter) per 32-byte block,
// XORed over the plaintext. consoleKey = SHA-256(OpenPSID || salt), so the same
// ciphertext only decrypts on the console that produced it.
//

#include "gdrive-crypto.h"

#include "string-utilities.h"
#include "dbg.h"
#include <stdint.h>
#include <sys/ss_get_open_psid.h>   // sys_ss_get_open_psid (console-unique id, syscall 872)
#include <sys/random_number.h>      // sys_get_random_number (nonce)

#define NONCE_LEN     16
#define SHA256_LEN    32
#define MAX_SECRET    1024   // fits the client id + secret + refresh token bundle; caps work buffers

// section: SHA-256 (compact, self-contained)

typedef struct { uint32_t state[8]; uint64_t bitCount; uint8_t buffer[64]; int bufferLen; } Sha256;

static uint32_t rotr32(uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }

static void sha256Block(Sha256 *ctx, const uint8_t *block)
{
   static const uint32_t k[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

   uint32_t w[64];
   for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
   for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
      uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
   }

   uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
   uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
   for (int i = 0; i < 64; i++) {
      uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + s1 + ch + k[i] + w[i];
      uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = s0 + maj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
   }
   ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
   ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256Init(Sha256 *ctx)
{
   ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
   ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
   ctx->bitCount = 0; ctx->bufferLen = 0;
}

static void sha256Update(Sha256 *ctx, const uint8_t *data, int len)
{
   for (int i = 0; i < len; i++) {
      ctx->buffer[ctx->bufferLen++] = data[i];
      ctx->bitCount += 8;
      if (ctx->bufferLen == 64) { sha256Block(ctx, ctx->buffer); ctx->bufferLen = 0; }
   }
}

static void sha256Final(Sha256 *ctx, uint8_t out[SHA256_LEN])
{
   uint64_t bits = ctx->bitCount;
   uint8_t pad = 0x80;
   sha256Update(ctx, &pad, 1);
   uint8_t zero = 0;
   while (ctx->bufferLen != 56) sha256Update(ctx, &zero, 1);
   uint8_t lengthBytes[8];
   for (int i = 0; i < 8; i++) lengthBytes[i] = (uint8_t)(bits >> (56 - i * 8));
   sha256Update(ctx, lengthBytes, 8);
   for (int i = 0; i < 8; i++) {
      out[i*4]   = (uint8_t)(ctx->state[i] >> 24);
      out[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
      out[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
      out[i*4+3] = (uint8_t)(ctx->state[i]);
   }
}

// section: console key + keystream

static const char KEY_SALT[] = "simple-file-manager/gdrive/v1";

// consoleKey = SHA-256(OpenPSID(16 bytes) || salt). returns -1 if the console id is unreadable.
static int consoleKey(uint8_t out[SHA256_LEN])
{
   CellSsOpenPSID psid;
   if (sys_ss_get_open_psid(&psid) != 0) return -1;
   uint8_t material[16];
   for (int i = 0; i < 8; i++) material[i]     = (uint8_t)(psid.high >> (56 - i * 8));
   for (int i = 0; i < 8; i++) material[8 + i] = (uint8_t)(psid.low  >> (56 - i * 8));

   Sha256 ctx;
   sha256Init(&ctx);
   sha256Update(&ctx, material, sizeof material);
   sha256Update(&ctx, (const uint8_t *)KEY_SALT, (int)sizeof KEY_SALT - 1);
   sha256Final(&ctx, out);
   return 0;
}

// XORs data in place with the SHA-256(key || nonce || counter) keystream (CTR-style).
static void applyKeystream(const uint8_t key[SHA256_LEN], const uint8_t nonce[NONCE_LEN], uint8_t *data, int len)
{
   uint32_t counter = 0;
   for (int offset = 0; offset < len; offset += SHA256_LEN, counter++) {
      uint8_t counterBytes[4] = { (uint8_t)(counter >> 24), (uint8_t)(counter >> 16), (uint8_t)(counter >> 8), (uint8_t)counter };
      uint8_t block[SHA256_LEN];
      Sha256 ctx;
      sha256Init(&ctx);
      sha256Update(&ctx, key, SHA256_LEN);
      sha256Update(&ctx, nonce, NONCE_LEN);
      sha256Update(&ctx, counterBytes, 4);
      sha256Final(&ctx, block);
      int chunk = len - offset < SHA256_LEN ? len - offset : SHA256_LEN;
      for (int i = 0; i < chunk; i++) data[offset + i] ^= block[i];
   }
}

// section: public API

int gdriveEncryptSecret(const char *plain, char *outHex, int outCap)
{
   int plainLen = getStrLen(plain);
   if (plainLen > MAX_SECRET) { logError("[gdrive] secret too long to encrypt\n"); return -1; }

   uint8_t key[SHA256_LEN];
   if (consoleKey(key) != 0) { logError("[gdrive] no console id; cannot encrypt\n"); return -1; }

   uint8_t blob[NONCE_LEN + MAX_SECRET];
   if (sys_get_random_number(blob, NONCE_LEN) != 0) { logError("[gdrive] no entropy for nonce\n"); return -1; }
   memCopy(blob + NONCE_LEN, plain, plainLen);
   applyKeystream(key, blob, blob + NONCE_LEN, plainLen);

   int blobLen = NONCE_LEN + plainLen;
   if (blobLen * 2 + 1 > outCap) { logError("[gdrive] encrypt output buffer too small\n"); return -1; }
   toHexText(outHex, outCap, blob, blobLen);
   return 0;
}

int gdriveDecryptSecret(const char *hex, char *outPlain, int outCap)
{
   uint8_t blob[NONCE_LEN + MAX_SECRET];
   int blobLen = fromHexText(hex, blob, sizeof blob);
   if (blobLen < NONCE_LEN) { logError("[gdrive] encrypted value malformed\n"); return -1; }

   uint8_t key[SHA256_LEN];
   if (consoleKey(key) != 0) { logError("[gdrive] no console id; cannot decrypt\n"); return -1; }

   int plainLen = blobLen - NONCE_LEN;
   if (plainLen + 1 > outCap) { logError("[gdrive] decrypt output buffer too small\n"); return -1; }
   applyKeystream(key, blob, blob + NONCE_LEN, plainLen);
   memCopy(outPlain, blob + NONCE_LEN, plainLen);
   outPlain[plainLen] = '\0';
   return 0;
}
