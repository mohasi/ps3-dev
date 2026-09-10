// dtls-certificate - see dtls-certificate.h.
//
// The certificate is written out by hand rather than by a library, because BearSSL reads
// certificates but does not write them. The format is a nest of length-prefixed pieces: every
// piece is a tag byte, then how many bytes follow, then those bytes. A piece that contains other
// pieces cannot know its own length until they are all written, so everything here is built
// innermost first and wrapped afterwards.

#include "dtls-certificate.h"

#include "dbg.h"
#include "string-utilities.h"

#include "bearssl.h"

#include <sys/random_number.h>

#define TAG "[tls] "

#define CERTIFICATE_MAX 640
#define PRIVATE_KEY_MAX BR_EC_KBUF_PRIV_MAX_SIZE
#define PUBLIC_KEY_MAX  BR_EC_KBUF_PUB_MAX_SIZE

// tags, from the encoding rules the format is written in
#define TAG_INTEGER      0x02
#define TAG_BIT_STRING   0x03
#define TAG_OBJECT_ID    0x06
#define TAG_UTF8_STRING  0x0C
#define TAG_SEQUENCE     0x30
#define TAG_SET          0x31
#define TAG_UTC_TIME     0x17
#define TAG_CONTEXT_0    0xA0

// the names of the things this certificate says about itself, each a fixed sequence of bytes the
// standard assigns: the signature algorithm, the kind of key, the curve, and the "common name"
// field the subject is written into
static const uint8_t OID_ECDSA_SHA256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const uint8_t OID_EC_PUBLIC_KEY[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const uint8_t OID_PRIME256V1[]    = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const uint8_t OID_COMMON_NAME[]   = { 0x55, 0x04, 0x03 };

static const char SUBJECT_NAME[] = "cell-stream";

// nothing checks these dates: the fingerprint is what vouches for the certificate. they are set
// wide enough that a console with a wrong clock still presents something inside its own validity.
static const char NOT_BEFORE[] = "200101000000Z";
static const char NOT_AFTER[]  = "300101000000Z";

static uint8_t certificate[CERTIFICATE_MAX];
static int certificateLength;
static char fingerprint[DTLS_FINGERPRINT_MAX];

static uint8_t privateKeyBuffer[PRIVATE_KEY_MAX];
static br_ec_private_key privateKey;

// section: writing the pieces

// a length is one byte when it is small, otherwise a count of bytes followed by them
static int writeLength(uint8_t *out, int at, int length)
{
   if (length < 0x80) {
      out[at++] = (uint8_t)length;
   } else if (length < 0x100) {
      out[at++] = 0x81;
      out[at++] = (uint8_t)length;
   } else {
      out[at++] = 0x82;
      out[at++] = (uint8_t)(length >> 8);
      out[at++] = (uint8_t)length;
   }
   return at;
}

static int writePiece(uint8_t *out, int at, uint8_t tag, const void *value, int length)
{
   out[at++] = tag;
   at = writeLength(out, at, length);
   memCopy(out + at, value, length);
   return at + length;
}

// wraps everything written since `contentStart` in a tag of its own, which means shifting it along
// to make room for the header now that its length is finally known
static int wrapFrom(uint8_t *out, int contentStart, int contentEnd, uint8_t tag)
{
   int length = contentEnd - contentStart;
   uint8_t header[4];
   int headerLength = writeLength(header, 1, length);
   header[0] = tag;

   for (int at = contentEnd - 1; at >= contentStart; at--) out[at + headerLength] = out[at];
   memCopy(out + contentStart, header, headerLength);
   return contentEnd + headerLength;
}

// an integer is signed, so a leading bit that is set needs a zero byte in front of it
static int writeInteger(uint8_t *out, int at, const uint8_t *value, int length)
{
   int needsPad = (value[0] & 0x80) != 0;
   out[at++] = TAG_INTEGER;
   at = writeLength(out, at, length + needsPad);
   if (needsPad) out[at++] = 0;
   memCopy(out + at, value, length);
   return at + length;
}

// SEQUENCE { OBJECT IDENTIFIER ecdsa-with-SHA256 }
static int writeSignatureAlgorithm(uint8_t *out, int at)
{
   int start = at;
   at = writePiece(out, at, TAG_OBJECT_ID, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256);
   return wrapFrom(out, start, at, TAG_SEQUENCE);
}

// the one-line name this certificate gives itself, on both sides since it signs itself
static int writeName(uint8_t *out, int at)
{
   int nameStart = at;
   int attributeStart = at;
   at = writePiece(out, at, TAG_OBJECT_ID, OID_COMMON_NAME, sizeof OID_COMMON_NAME);
   at = writePiece(out, at, TAG_UTF8_STRING, SUBJECT_NAME, (int)sizeof SUBJECT_NAME - 1);
   at = wrapFrom(out, attributeStart, at, TAG_SEQUENCE);
   at = wrapFrom(out, attributeStart, at, TAG_SET);
   return wrapFrom(out, nameStart, at, TAG_SEQUENCE);
}

// SEQUENCE { SEQUENCE { id-ecPublicKey, prime256v1 }, BIT STRING publicKey }
static int writePublicKey(uint8_t *out, int at, const br_ec_public_key *publicKey)
{
   int start = at;

   int algorithmStart = at;
   at = writePiece(out, at, TAG_OBJECT_ID, OID_EC_PUBLIC_KEY, sizeof OID_EC_PUBLIC_KEY);
   at = writePiece(out, at, TAG_OBJECT_ID, OID_PRIME256V1, sizeof OID_PRIME256V1);
   at = wrapFrom(out, algorithmStart, at, TAG_SEQUENCE);

   // a bit string counts unused trailing bits, and a key never has any
   out[at++] = TAG_BIT_STRING;
   at = writeLength(out, at, (int)publicKey->qlen + 1);
   out[at++] = 0;
   memCopy(out + at, publicKey->q, publicKey->qlen);
   at += (int)publicKey->qlen;

   return wrapFrom(out, start, at, TAG_SEQUENCE);
}

// everything the signature is taken over
static int writeBody(uint8_t *out, int at, const br_ec_public_key *publicKey, const uint8_t serial[8])
{
   int start = at;

   int versionStart = at;
   uint8_t version = 2;   // v3, which is what a certificate with no extensions still declares
   at = writePiece(out, at, TAG_INTEGER, &version, 1);
   at = wrapFrom(out, versionStart, at, TAG_CONTEXT_0);

   at = writeInteger(out, at, serial, 8);
   at = writeSignatureAlgorithm(out, at);
   at = writeName(out, at);

   int validityStart = at;
   at = writePiece(out, at, TAG_UTC_TIME, NOT_BEFORE, (int)sizeof NOT_BEFORE - 1);
   at = writePiece(out, at, TAG_UTC_TIME, NOT_AFTER, (int)sizeof NOT_AFTER - 1);
   at = wrapFrom(out, validityStart, at, TAG_SEQUENCE);

   at = writeName(out, at);
   at = writePublicKey(out, at, publicKey);

   return wrapFrom(out, start, at, TAG_SEQUENCE);
}

// section: making one

void writeCertificateFingerprint(const uint8_t *der, int length, char *out)
{
   br_sha256_context sha;
   uint8_t digest[br_sha256_SIZE];
   br_sha256_init(&sha);
   br_sha256_update(&sha, der, length);
   br_sha256_out(&sha, digest);

   static const char HEX[] = "0123456789ABCDEF";
   int at = 0;
   for (int byte = 0; byte < br_sha256_SIZE; byte++) {
      if (byte) out[at++] = ':';
      out[at++] = HEX[digest[byte] >> 4];
      out[at++] = HEX[digest[byte] & 15];
   }
   out[at] = 0;
}

int createDtlsCertificate(void)
{
   // BearSSL wants a generator it understands, so the console's own randomness seeds one
   uint8_t seed[32];
   if (sys_get_random_number(seed, sizeof seed) != 0) {
      logError(TAG "dtls: the console would not produce randomness for a key\n");
      return -1;
   }

   br_hmac_drbg_context generator;
   br_hmac_drbg_init(&generator, &br_sha256_vtable, seed, sizeof seed);

   const br_ec_impl *curve = br_ec_get_default();
   if (br_ec_keygen(&generator.vtable, curve, &privateKey, privateKeyBuffer, BR_EC_secp256r1) == 0) {
      logError(TAG "dtls: could not make a key\n");
      return -1;
   }

   uint8_t publicKeyBuffer[PUBLIC_KEY_MAX];
   br_ec_public_key publicKey;
   if (br_ec_compute_pub(curve, &publicKey, publicKeyBuffer, &privateKey) == 0) {
      logError(TAG "dtls: could not derive the public half of the key\n");
      return -1;
   }

   uint8_t serial[8];
   if (sys_get_random_number(serial, sizeof serial) != 0) return -1;
   serial[0] &= 0x7F;   // a serial number is a positive integer

   // the body first, because the signature is taken over it
   int bodyStart = 0;
   int bodyEnd = writeBody(certificate, bodyStart, &publicKey, serial);

   br_sha256_context sha;
   uint8_t digest[br_sha256_SIZE];
   br_sha256_init(&sha);
   br_sha256_update(&sha, certificate + bodyStart, bodyEnd - bodyStart);
   br_sha256_out(&sha, digest);

   uint8_t signature[80];
   size_t signatureLength = br_ecdsa_sign_asn1_get_default()(curve, &br_sha256_vtable, digest, &privateKey, signature);
   if (signatureLength == 0) {
      logError(TAG "dtls: could not sign the certificate\n");
      return -1;
   }

   int at = bodyEnd;
   at = writeSignatureAlgorithm(certificate, at);

   certificate[at++] = TAG_BIT_STRING;
   at = writeLength(certificate, at, (int)signatureLength + 1);
   certificate[at++] = 0;
   memCopy(certificate + at, signature, signatureLength);
   at += (int)signatureLength;

   certificateLength = wrapFrom(certificate, 0, at, TAG_SEQUENCE);
   if (certificateLength > CERTIFICATE_MAX) {
      logError(TAG "dtls: the certificate came out longer than its buffer\n");
      return -1;
   }

   writeCertificateFingerprint(certificate, certificateLength, fingerprint);

   logInfo(TAG "dtls: certificate ready, %d bytes, fingerprint %s\n", certificateLength, fingerprint);
   return 0;
}

const uint8_t *getDtlsCertificate(int *length)
{
   if (length) *length = certificateLength;
   return certificateLength > 0 ? certificate : 0;
}

const char *getDtlsFingerprint(void) { return fingerprint; }

int signWithDtlsKey(const uint8_t *digest, uint8_t *out, int capacity)
{
   if (certificateLength == 0 || capacity < 80) return 0;
   return (int)br_ecdsa_sign_asn1_get_default()(br_ec_get_default(), &br_sha256_vtable, digest, &privateKey, out);
}
