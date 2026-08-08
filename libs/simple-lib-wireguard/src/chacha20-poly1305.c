#include "chacha20-poly1305.h"

#include "chacha20.h"
#include "poly1305.h"
#include "string-utilities.h"   // memSet
#include "wg-bytes.h"

static const uint8_t zeroPadding[16] = { 0 };

// the authenticated data and the ciphertext are each padded to a 16-byte boundary, then the two
// lengths are appended. RFC 8439 section 2.8.1.
static void computeTag(uint8_t tag[AEAD_TAG_LENGTH], const uint8_t *aad, int aadLength, const uint8_t *cipher,
                       int cipherLength, const uint8_t nonce[AEAD_NONCE_LENGTH], const uint8_t key[AEAD_KEY_LENGTH])
{
   uint8_t keyStreamBlock[CHACHA20_BLOCK_LENGTH];
   getChaCha20Block(keyStreamBlock, key, nonce, 0);

   Poly1305State state;
   initPoly1305(&state, keyStreamBlock);

   if (aadLength > 0) {
      updatePoly1305(&state, aad, aadLength);
      updatePoly1305(&state, zeroPadding, (16 - (aadLength & 15)) & 15);
   }
   if (cipherLength > 0) {
      updatePoly1305(&state, cipher, cipherLength);
      updatePoly1305(&state, zeroPadding, (16 - (cipherLength & 15)) & 15);
   }

   uint8_t lengths[16];
   store64le(lengths, (uint64_t)aadLength);
   store64le(lengths + 8, (uint64_t)cipherLength);
   updatePoly1305(&state, lengths, sizeof lengths);

   finishPoly1305(&state, tag);
   memSet(keyStreamBlock, 0, sizeof keyStreamBlock);
}

void sealChaCha20Poly1305(uint8_t *out, const uint8_t *plain, int plainLength, const uint8_t *aad, int aadLength,
                          const uint8_t nonce[AEAD_NONCE_LENGTH], const uint8_t key[AEAD_KEY_LENGTH])
{
   // block counter 0 makes the Poly1305 key, so the message starts at 1
   xorChaCha20(out, plain, plainLength, key, nonce, 1);
   computeTag(out + plainLength, aad, aadLength, out, plainLength, nonce, key);
}

int openChaCha20Poly1305(uint8_t *out, const uint8_t *sealed, int sealedLength, const uint8_t *aad, int aadLength,
                         const uint8_t nonce[AEAD_NONCE_LENGTH], const uint8_t key[AEAD_KEY_LENGTH])
{
   if (sealedLength < AEAD_TAG_LENGTH) return 0;
   int cipherLength = sealedLength - AEAD_TAG_LENGTH;

   uint8_t expectedTag[AEAD_TAG_LENGTH];
   computeTag(expectedTag, aad, aadLength, sealed, cipherLength, nonce, key);
   if (!bytesEqual(expectedTag, sealed + cipherLength, AEAD_TAG_LENGTH)) return 0;

   xorChaCha20(out, sealed, cipherLength, key, nonce, 1);
   return 1;
}
