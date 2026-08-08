#include "x25519.h"

#include "string-utilities.h"   // memSet
#include "wg-bytes.h"

// Field elements are 16 limbs of 16 bits held in 64-bit signed words, so every product and every
// intermediate sum fits without needing a 128-bit type (GCC 4.1.1 has none). A 51-bit limb layout
// would be several times faster, but a handshake happens about once every two minutes while
// ChaCha20 runs on every packet, so this is not where the time goes. Revisit in the performance
// pass, not before.
//
// The ladder below is the constant-time Montgomery ladder of RFC 7748 section 5: the same
// operations run for every bit of the scalar, and the two working points are swapped by masking
// rather than by branching, so timing does not depend on the private key.

#define FIELD_LIMBS 16

typedef int64_t Field[FIELD_LIMBS];

static const Field curveConstant121665 = { 0xDB41, 1 };

static void copyField(Field out, const Field in)
{
   for (int index = 0; index < FIELD_LIMBS; index++) out[index] = in[index];
}

static void setFieldZero(Field out)
{
   for (int index = 0; index < FIELD_LIMBS; index++) out[index] = 0;
}

// bring every limb back into 0..65535, folding the overflow above limb 15 back into limb 0.
// 2^256 = 38 modulo 2^255 - 19, which is where the 38 comes from.
static void carryField(Field value)
{
   for (int index = 0; index < FIELD_LIMBS; index++) {
      int64_t carry = value[index] >> 16;
      value[index] -= carry << 16;
      if (index < FIELD_LIMBS - 1) value[index + 1] += carry;
      else value[0] += 38 * carry;
   }
}

static void addField(Field out, const Field a, const Field b)
{
   for (int index = 0; index < FIELD_LIMBS; index++) out[index] = a[index] + b[index];
}

static void subtractField(Field out, const Field a, const Field b)
{
   for (int index = 0; index < FIELD_LIMBS; index++) out[index] = a[index] - b[index];
}

static void multiplyField(Field out, const Field a, const Field b)
{
   int64_t product[2 * FIELD_LIMBS - 1];
   for (int index = 0; index < 2 * FIELD_LIMBS - 1; index++) product[index] = 0;

   for (int left = 0; left < FIELD_LIMBS; left++)
      for (int right = 0; right < FIELD_LIMBS; right++) product[left + right] += a[left] * b[right];

   for (int index = 0; index < FIELD_LIMBS - 1; index++) product[index] += 38 * product[index + FIELD_LIMBS];
   for (int index = 0; index < FIELD_LIMBS; index++) out[index] = product[index];

   carryField(out);
   carryField(out);
}

static void squareField(Field out, const Field in)
{
   multiplyField(out, in, in);
}

// swap a and b when doSwap is 1, without branching on it
static void selectSwap(Field a, Field b, int64_t doSwap)
{
   int64_t mask = ~(doSwap - 1);
   for (int index = 0; index < FIELD_LIMBS; index++) {
      int64_t difference = mask & (a[index] ^ b[index]);
      a[index] ^= difference;
      b[index] ^= difference;
   }
}

// inverse by exponentiation to p - 2, which is all ones except at bit positions 2 and 4
static void invertField(Field out, const Field in)
{
   Field work;
   copyField(work, in);
   for (int bit = 253; bit >= 0; bit--) {
      squareField(work, work);
      if (bit != 2 && bit != 4) multiplyField(work, work, in);
   }
   copyField(out, work);
}

static void unpackField(Field out, const uint8_t bytes[32])
{
   for (int index = 0; index < FIELD_LIMBS; index++)
      out[index] = (int64_t)bytes[2 * index] | ((int64_t)bytes[2 * index + 1] << 8);
   out[15] &= 0x7FFF;   // the top bit of a u-coordinate is ignored, RFC 7748 section 5
}

static void packField(uint8_t bytes[32], const Field in)
{
   Field value, reduced;
   copyField(value, in);
   carryField(value);
   carryField(value);
   carryField(value);

   // subtract p; keep the result only if it did not borrow. twice, because one pass can leave a
   // value still just above p.
   for (int pass = 0; pass < 2; pass++) {
      reduced[0] = value[0] - 0xFFED;
      for (int index = 1; index < FIELD_LIMBS - 1; index++) {
         reduced[index] = value[index] - 0xFFFF - ((reduced[index - 1] >> 16) & 1);
         reduced[index - 1] &= 0xFFFF;
      }
      reduced[15] = value[15] - 0x7FFF - ((reduced[14] >> 16) & 1);
      int64_t borrowed = (reduced[15] >> 16) & 1;
      reduced[14] &= 0xFFFF;
      selectSwap(value, reduced, 1 - borrowed);
   }

   for (int index = 0; index < FIELD_LIMBS; index++) {
      bytes[2 * index] = (uint8_t)(value[index] & 0xFF);
      bytes[2 * index + 1] = (uint8_t)((value[index] >> 8) & 0xFF);
   }
}

static void multiplyByScalar(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
   // clamp the scalar, RFC 7748 section 5
   uint8_t clamped[32];
   for (int index = 0; index < 32; index++) clamped[index] = scalar[index];
   clamped[0] &= 248;
   clamped[31] = (uint8_t)((clamped[31] & 127) | 64);

   // the two ladder points, named as in RFC 7748: (x2, z2) and (x3, z3) are projective
   // u-coordinates that stay one scalar bit apart.
   Field base, x2, z2, x3, z3, temp1, temp2;
   unpackField(base, point);
   copyField(x3, base);
   setFieldZero(x2);
   setFieldZero(z2);
   setFieldZero(z3);
   x2[0] = 1;
   z3[0] = 1;

   for (int bit = 254; bit >= 0; bit--) {
      int64_t bitSet = (clamped[bit >> 3] >> (bit & 7)) & 1;
      selectSwap(x2, x3, bitSet);
      selectSwap(z2, z3, bitSet);

      addField(temp1, x2, z2);
      subtractField(x2, x2, z2);
      addField(z2, x3, z3);
      subtractField(x3, x3, z3);
      squareField(z3, temp1);
      squareField(temp2, x2);
      multiplyField(x2, z2, x2);
      multiplyField(z2, x3, temp1);
      addField(temp1, x2, z2);
      subtractField(x2, x2, z2);
      squareField(x3, x2);
      subtractField(z2, z3, temp2);
      multiplyField(x2, z2, curveConstant121665);
      addField(x2, x2, z3);
      multiplyField(z2, z2, x2);
      multiplyField(x2, z3, temp2);
      multiplyField(z3, x3, base);
      squareField(x3, temp1);

      selectSwap(x2, x3, bitSet);
      selectSwap(z2, z3, bitSet);
   }

   invertField(z2, z2);
   multiplyField(x2, x2, z2);
   packField(out, x2);

   memSet(clamped, 0, sizeof clamped);
}

void getX25519PublicKey(uint8_t publicKey[X25519_KEY_LENGTH], const uint8_t privateKey[X25519_KEY_LENGTH])
{
   uint8_t basePoint[32];
   memSet(basePoint, 0, sizeof basePoint);
   basePoint[0] = 9;
   multiplyByScalar(publicKey, privateKey, basePoint);
}

int computeX25519Shared(uint8_t shared[X25519_KEY_LENGTH], const uint8_t privateKey[X25519_KEY_LENGTH],
                        const uint8_t peerPublicKey[X25519_KEY_LENGTH])
{
   multiplyByScalar(shared, privateKey, peerPublicKey);

   uint8_t combined = 0;
   for (int index = 0; index < X25519_KEY_LENGTH; index++) combined |= shared[index];
   return combined != 0;
}
