// dtls - see dtls.h.
//
// Messages are built by hand into a byte buffer. Every number on the wire is written most
// significant byte first, whatever the machine underneath does, so all of it goes out a byte at a
// time rather than by copying a struct.
//
// Both sides run a hash over every message of the exchange, in order, and later sign and check that
// hash. So each message is fed to the hash as it is written or read, and the far end's repeats are
// dropped before they reach it: a message counted twice would leave the two hashes different.

#include "dtls.h"

#include "dbg.h"
#include "dtls-certificate.h"
#include "string-utilities.h"

#include "bearssl.h"

#include <sys/random_number.h>

#define TAG "[tls] "

#define DTLS_1_2 0xFEFD   // the version number is counted backwards, so 1.2 is written as 254.253

// what a record carries, from the TLS record layer
#define RECORD_CHANGE_CIPHER_SPEC 20
#define RECORD_ALERT              21
#define RECORD_HANDSHAKE          22
#define RECORD_APPLICATION_DATA   23

// the messages of the exchange itself
#define HANDSHAKE_CLIENT_HELLO         1
#define HANDSHAKE_SERVER_HELLO         2
#define HANDSHAKE_HELLO_VERIFY_REQUEST 3
#define HANDSHAKE_CERTIFICATE          11
#define HANDSHAKE_SERVER_KEY_EXCHANGE  12
#define HANDSHAKE_CERTIFICATE_REQUEST  13
#define HANDSHAKE_SERVER_HELLO_DONE    14
#define HANDSHAKE_CERTIFICATE_VERIFY   15
#define HANDSHAKE_CLIENT_KEY_EXCHANGE  16
#define HANDSHAKE_FINISHED             20

// cipher suites, by the numbers the IANA TLS registry assigns them. All four agree a shared secret
// with a fresh key per connection, which is what the far end is expected to insist on.
static const uint16_t CIPHER_SUITES[] = {
   0xC02B,   // ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
   0xC02F,   // ECDHE_RSA_WITH_AES_128_GCM_SHA256
   0xC009,   // ECDHE_ECDSA_WITH_AES_128_CBC_SHA
   0xC013    // ECDHE_RSA_WITH_AES_128_CBC_SHA
};

// extension numbers, also from the IANA TLS registry
#define EXTENSION_SUPPORTED_GROUPS       10
#define EXTENSION_EC_POINT_FORMATS       11
#define EXTENSION_SIGNATURE_ALGORITHMS   13
#define EXTENSION_USE_SRTP               14
#define EXTENSION_EXTENDED_MASTER_SECRET 23

#define GROUP_SECP256R1 0x0017             // the curve, from the IANA TLS registry
#define SRTP_AES128_CM_HMAC_SHA1_80 0x0001 // the media cipher, from RFC 5764

// how a signature is described in TLS 1.2: a hash and a key type, one byte each
#define HASH_SHA256    4
#define SIGNATURE_ECDSA 3
#define SIGNATURE_RSA   1

#define COOKIE_MAX     32    // RFC 6347 caps a cookie at 32 bytes
#define FLIGHT_MAX     1400  // one datagram, kept under a normal network's limit
#define RANDOM_SIZE    32
#define POINT_MAX      BR_EC_KBUF_PUB_MAX_SIZE
#define SIGNATURE_MAX  80
#define RECORD_HEADER    13  // type, version, epoch, sequence, length
#define HANDSHAKE_HEADER 12  // type, length, sequence, fragment offset, fragment length

// the agreed cipher encrypts with a 16 byte key and a 12 byte number that is never reused: 4 bytes
// of it come from the key material, the other 8 travel with each record. it adds a 16 byte tag.
#define KEY_SIZE      16
#define SALT_SIZE     4
#define NONCE_SIZE    8
#define TAG_SIZE      16
#define MASTER_SIZE   48
#define VERIFY_SIZE   12
#define KEY_BLOCK_SIZE (2 * KEY_SIZE + 2 * SALT_SIZE)

static DtlsSendFunc sendDatagram;
static char expectedFingerprint[DTLS_FINGERPRINT_MAX];   // what the description said we would find

static uint8_t clientRandom[RANDOM_SIZE];
static uint8_t serverRandom[RANDOM_SIZE];
static uint8_t cookie[COOKIE_MAX];
static int cookieLength;

static uint8_t serverPoint[POINT_MAX];   // the far end's half of the shared secret
static int serverPointLength;

static uint8_t ourPoint[POINT_MAX];      // our half, thrown away when the connection ends
static int ourPointLength;
static uint8_t ephemeralKeyBuffer[BR_EC_KBUF_PRIV_MAX_SIZE];
static br_ec_private_key ephemeralKey;

static br_sha256_context transcript;
static br_sha256_context expectedTranscript;   // the hash as it stood before the message being read
static uint8_t sessionHash[br_sha256_SIZE];   // the conversation up to the point the secret is agreed

static uint8_t masterSecret[MASTER_SIZE];
static uint8_t writeKey[KEY_SIZE], readKey[KEY_SIZE];
static uint8_t writeSalt[SALT_SIZE], readSalt[SALT_SIZE];
static uint8_t srtpKeyBlock[2 * (DTLS_MEDIA_KEY_SIZE + DTLS_MEDIA_SALT_SIZE)];

static int extendedMasterSecret;   // whether the far end tied the secret to this conversation
static uint16_t srtpProfile;       // the media cipher it agreed to

static uint64_t encryptedSequence;   // records counted again from zero once encryption starts

static uint16_t messageSequence;    // counts the messages we send, so the far end can order them
static uint16_t expectedSequence;   // the next one we have not seen from the far end
static uint64_t recordSequence;     // counts the records we send, and is never reused

static uint8_t flight[FLIGHT_MAX];  // what we last sent, kept in case it has to go again
static int flightLength;

// section: writing numbers

static int writeByte(uint8_t *out, int at, uint8_t value)
{
   out[at] = value;
   return at + 1;
}

static int write16(uint8_t *out, int at, uint16_t value)
{
   out[at] = (uint8_t)(value >> 8);
   out[at + 1] = (uint8_t)value;
   return at + 2;
}

static int write24(uint8_t *out, int at, uint32_t value)
{
   out[at] = (uint8_t)(value >> 16);
   out[at + 1] = (uint8_t)(value >> 8);
   out[at + 2] = (uint8_t)value;
   return at + 3;
}

static int writeBytes(uint8_t *out, int at, const void *data, int length)
{
   memCopy(out + at, data, length);
   return at + length;
}

static uint16_t read16(const uint8_t *data) { return (uint16_t)((data[0] << 8) | data[1]); }
static uint32_t read24(const uint8_t *data) { return ((uint32_t)data[0] << 16) | (uint32_t)read16(data + 1); }

// section: building our messages

// the list of things we are willing to do, each one a number and its own length
static int writeExtensions(uint8_t *out, int at)
{
   int lengthAt = at;
   at += 2;   // filled in once the extensions below are written

   // the one curve we can do
   at = write16(out, at, EXTENSION_SUPPORTED_GROUPS);
   at = write16(out, at, 4);
   at = write16(out, at, 2);
   at = write16(out, at, GROUP_SECP256R1);

   // how a point on that curve is written: whole, rather than shortened
   at = write16(out, at, EXTENSION_EC_POINT_FORMATS);
   at = write16(out, at, 2);
   at = writeByte(out, at, 1);
   at = writeByte(out, at, 0);

   // which signatures we can check, as a hash and a key type per pair
   at = write16(out, at, EXTENSION_SIGNATURE_ALGORITHMS);
   at = write16(out, at, 6);
   at = write16(out, at, 4);
   at = writeByte(out, at, HASH_SHA256); at = writeByte(out, at, SIGNATURE_ECDSA);
   at = writeByte(out, at, HASH_SHA256); at = writeByte(out, at, SIGNATURE_RSA);

   // that this exchange is also agreeing the keys the media will be encrypted with
   at = write16(out, at, EXTENSION_USE_SRTP);
   at = write16(out, at, 5);
   at = write16(out, at, 2);
   at = write16(out, at, SRTP_AES128_CM_HMAC_SHA1_80);
   at = writeByte(out, at, 0);   // no key identifier

   // ties the agreed secret to this exchange, so it cannot be lifted into another one
   at = write16(out, at, EXTENSION_EXTENDED_MASTER_SECRET);
   at = write16(out, at, 0);

   write16(out, lengthAt, (uint16_t)(at - lengthAt - 2));
   return at;
}

static int writeClientHello(uint8_t *out, int at)
{
   at = write16(out, at, DTLS_1_2);
   at = writeBytes(out, at, clientRandom, RANDOM_SIZE);

   at = writeByte(out, at, 0);   // no earlier session to resume

   at = writeByte(out, at, (uint8_t)cookieLength);
   at = writeBytes(out, at, cookie, cookieLength);

   at = write16(out, at, (uint16_t)sizeof CIPHER_SUITES);
   for (int suite = 0; suite < (int)(sizeof CIPHER_SUITES / sizeof CIPHER_SUITES[0]); suite++)
      at = write16(out, at, CIPHER_SUITES[suite]);

   at = writeByte(out, at, 1);   // one way of compressing offered
   at = writeByte(out, at, 0);   // and it is "not at all"

   return writeExtensions(out, at);
}

// our certificate, as a list of one: nothing signed it, so there is no chain above it
static int writeCertificate(uint8_t *out, int at)
{
   int length = 0;
   const uint8_t *certificate = getDtlsCertificate(&length);
   if (!certificate) {
      logError(TAG "dtls: there is no certificate to present\n");
      return at;
   }

   at = write24(out, at, (uint32_t)length + 3);
   at = write24(out, at, (uint32_t)length);
   return writeBytes(out, at, certificate, length);
}

// our half of the shared secret
static int writeClientKeyExchange(uint8_t *out, int at)
{
   at = writeByte(out, at, (uint8_t)ourPointLength);
   return writeBytes(out, at, ourPoint, ourPointLength);
}

// proves the certificate above is ours, by signing everything said so far with its key
static int writeCertificateVerify(uint8_t *out, int at)
{
   br_sha256_context sofar = transcript;   // the hash keeps running, so sign a copy of it
   uint8_t digest[br_sha256_SIZE];
   br_sha256_out(&sofar, digest);

   uint8_t signature[SIGNATURE_MAX];
   int signatureLength = signWithDtlsKey(digest, signature, sizeof signature);
   if (signatureLength == 0) {
      logError(TAG "dtls: could not sign for the far end\n");
      return at;
   }

   at = writeByte(out, at, HASH_SHA256);
   at = writeByte(out, at, SIGNATURE_ECDSA);
   at = write16(out, at, (uint16_t)signatureLength);
   return writeBytes(out, at, signature, signatureLength);
}

// section: agreeing the keys

// runs the standard's key stretcher: a secret and some named inputs in, as many bytes out as asked
static void stretch(uint8_t *out, int length, const uint8_t *secret, int secretLength, const char *label,
                    const uint8_t *first, int firstLength, const uint8_t *second, int secondLength)
{
   br_tls_prf_seed_chunk chunks[2];
   chunks[0].data = first;
   chunks[0].len = (size_t)firstLength;
   chunks[1].data = second;
   chunks[1].len = (size_t)secondLength;
   br_tls12_sha256_prf(out, (size_t)length, secret, (size_t)secretLength, label, second ? 2 : 1, chunks);
}

// multiplies the far end's point by our private half. both sides reach the same answer, and nobody
// watching the exchange can, which is the reason for doing it this way.
static int agreeSecret(uint8_t *premaster, int *premasterLength)
{
   uint8_t shared[POINT_MAX];
   memCopy(shared, serverPoint, serverPointLength);

   const br_ec_impl *curve = br_ec_get_default();
   if (curve->mul(shared, (size_t)serverPointLength, ephemeralKey.x, ephemeralKey.xlen, BR_EC_secp256r1) != 1) {
      logError(TAG "dtls: its key and ours would not combine\n");
      return -1;
   }

   // only the first coordinate of the result is the secret, and the leading byte says the point is
   // written whole rather than shortened, so it is not part of it either
   *premasterLength = (serverPointLength - 1) / 2;
   memCopy(premaster, shared + 1, *premasterLength);
   return 0;
}

static int deriveKeys(void)
{
   uint8_t premaster[POINT_MAX];
   int premasterLength = 0;
   if (agreeSecret(premaster, &premasterLength) != 0) return -1;

   if (extendedMasterSecret)
      stretch(masterSecret, MASTER_SIZE, premaster, premasterLength, "extended master secret",
              sessionHash, sizeof sessionHash, 0, 0);
   else
      stretch(masterSecret, MASTER_SIZE, premaster, premasterLength, "master secret",
              clientRandom, RANDOM_SIZE, serverRandom, RANDOM_SIZE);

   uint8_t keyBlock[KEY_BLOCK_SIZE];
   stretch(keyBlock, sizeof keyBlock, masterSecret, MASTER_SIZE, "key expansion",
           serverRandom, RANDOM_SIZE, clientRandom, RANDOM_SIZE);

   memCopy(writeKey, keyBlock, KEY_SIZE);
   memCopy(readKey, keyBlock + KEY_SIZE, KEY_SIZE);
   memCopy(writeSalt, keyBlock + 2 * KEY_SIZE, SALT_SIZE);
   memCopy(readSalt, keyBlock + 2 * KEY_SIZE + SALT_SIZE, SALT_SIZE);

   // the media is encrypted with its own keys, taken from the same secret under a different name
   stretch(srtpKeyBlock, sizeof srtpKeyBlock, masterSecret, MASTER_SIZE, "EXTRACTOR-dtls_srtp",
           clientRandom, RANDOM_SIZE, serverRandom, RANDOM_SIZE);
   return 0;
}

// what the tag is computed over besides the content: the record's own header fields
static void buildAssociatedData(uint8_t *out, const uint8_t *sequence, int type, int plainLength)
{
   memCopy(out, sequence, NONCE_SIZE);
   out[8] = (uint8_t)type;
   write16(out, 9, DTLS_1_2);
   write16(out, 11, (uint16_t)plainLength);
}

// the cipher both directions use, one record at a time. encrypting fills tag; decrypting checks it
// and reports through tagMatched.
static void runGcm(const uint8_t *key, const uint8_t *salt, const uint8_t *sequence, const uint8_t *associated,
                   int encrypt, uint8_t *data, int length, uint8_t *tag, uint32_t *tagMatched)
{
   uint8_t nonce[SALT_SIZE + NONCE_SIZE];   // never the same twice: the record's own count is in it
   memCopy(nonce, salt, SALT_SIZE);
   memCopy(nonce + SALT_SIZE, sequence, NONCE_SIZE);

   br_aes_ct_ctr_keys block;
   br_aes_ct_ctr_init(&block, key, KEY_SIZE);

   br_gcm_context gcm;
   br_gcm_init(&gcm, &block.vtable, br_ghash_ctmul32);
   br_gcm_reset(&gcm, nonce, sizeof nonce);
   br_gcm_aad_inject(&gcm, associated, RECORD_HEADER);
   br_gcm_flip(&gcm);
   br_gcm_run(&gcm, encrypt, data, (size_t)length);

   if (encrypt) br_gcm_get_tag(&gcm, tag);
   else *tagMatched = br_gcm_check_tag(&gcm, tag);
}

// section: sending

// appends one message, in a record of its own, to what will go out next
static int appendHandshake(int type, int (*writeBody)(uint8_t *, int))
{
   int at = flightLength;
   if (at + RECORD_HEADER + HANDSHAKE_HEADER > FLIGHT_MAX) return -1;

   at = writeByte(flight, at, RECORD_HANDSHAKE);
   at = write16(flight, at, DTLS_1_2);
   at = write16(flight, at, 0);   // epoch: still unencrypted
   at = write24(flight, at, (uint32_t)(recordSequence >> 24));
   at = write24(flight, at, (uint32_t)(recordSequence & 0xFFFFFF));
   int recordLengthAt = at;
   at += 2;

   int messageStart = at;
   at = writeByte(flight, at, (uint8_t)type);
   int bodyLengthAt = at;
   at += 3;
   at = write16(flight, at, messageSequence);
   at = write24(flight, at, 0);   // this message is not split across datagrams
   int fragmentLengthAt = at;
   at += 3;

   int contentStart = at;
   at = writeBody(flight, at);
   if (at > FLIGHT_MAX) {
      logError(TAG "dtls: the message came out longer than one datagram\n");
      return -1;
   }

   int contentLength = at - contentStart;
   write24(flight, bodyLengthAt, (uint32_t)contentLength);
   write24(flight, fragmentLengthAt, (uint32_t)contentLength);
   write16(flight, recordLengthAt, (uint16_t)(at - messageStart));

   br_sha256_update(&transcript, flight + messageStart, HANDSHAKE_HEADER + contentLength);

   flightLength = at;
   messageSequence++;
   recordSequence++;
   return 0;
}

// says that everything after it is encrypted. it is not part of the exchange's running hash.
static int appendChangeCipherSpec(void)
{
   int at = flightLength;
   if (at + RECORD_HEADER + 1 > FLIGHT_MAX) return -1;

   at = writeByte(flight, at, RECORD_CHANGE_CIPHER_SPEC);
   at = write16(flight, at, DTLS_1_2);
   at = write16(flight, at, 0);
   at = write24(flight, at, (uint32_t)(recordSequence >> 24));
   at = write24(flight, at, (uint32_t)(recordSequence & 0xFFFFFF));
   at = write16(flight, at, 1);
   at = writeByte(flight, at, 1);

   recordSequence++;
   flightLength = at;
   return 0;
}

// the last message of our half, and the first encrypted one. its content is a short value derived
// from the secret and from everything said so far, so the far end can tell that both matched.
static int appendFinished(void)
{
   br_sha256_context sofar = transcript;
   uint8_t digest[br_sha256_SIZE];
   br_sha256_out(&sofar, digest);

   uint8_t message[HANDSHAKE_HEADER + VERIFY_SIZE];
   int at = writeByte(message, 0, HANDSHAKE_FINISHED);
   at = write24(message, at, VERIFY_SIZE);
   at = write16(message, at, messageSequence);
   at = write24(message, at, 0);
   at = write24(message, at, VERIFY_SIZE);
   stretch(message + at, VERIFY_SIZE, masterSecret, MASTER_SIZE, "client finished", digest, sizeof digest, 0, 0);

   br_sha256_update(&transcript, message, sizeof message);
   messageSequence++;

   int recordAt = flightLength;
   if (recordAt + RECORD_HEADER + NONCE_SIZE + (int)sizeof message + TAG_SIZE > FLIGHT_MAX) return -1;

   // the count travels in the record header and again in front of the content, because the far end
   // needs it to build the same number we encrypted with
   uint8_t sequence[NONCE_SIZE];
   write16(sequence, 0, 1);   // epoch 1: encrypted
   write24(sequence, 2, (uint32_t)(encryptedSequence >> 24));
   write24(sequence, 5, (uint32_t)(encryptedSequence & 0xFFFFFF));

   at = writeByte(flight, recordAt, RECORD_HANDSHAKE);
   at = write16(flight, at, DTLS_1_2);
   at = writeBytes(flight, at, sequence, NONCE_SIZE);
   at = write16(flight, at, (uint16_t)(NONCE_SIZE + sizeof message + TAG_SIZE));
   at = writeBytes(flight, at, sequence, NONCE_SIZE);

   int contentAt = at;
   at = writeBytes(flight, at, message, sizeof message);

   uint8_t associated[RECORD_HEADER];
   buildAssociatedData(associated, sequence, RECORD_HANDSHAKE, (int)sizeof message);
   runGcm(writeKey, writeSalt, sequence, associated, 1, flight + contentAt, (int)sizeof message,
          flight + at, 0);
   at += TAG_SIZE;

   encryptedSequence++;
   flightLength = at;
   return 0;
}

static int sendFlight(void)
{
   if (sendDatagram(flight, flightLength) < 0) {
      logError(TAG "dtls: could not send\n");
      return -1;
   }
   return 0;
}

static int sendClientHello(void)
{
   flightLength = 0;
   if (appendHandshake(HANDSHAKE_CLIENT_HELLO, writeClientHello) != 0) return -1;
   return sendFlight();
}

// everything the far end asked for, in one go: who we are, our half of the secret, and a signature
// over the whole conversation that ties the two together
static int sendOurFlight(void)
{
   flightLength = 0;
   if (appendHandshake(HANDSHAKE_CERTIFICATE, writeCertificate) != 0) return -1;
   if (appendHandshake(HANDSHAKE_CLIENT_KEY_EXCHANGE, writeClientKeyExchange) != 0) return -1;

   // the secret is tied to the conversation as it stands here, with both halves of the key sent
   // and nothing after them, so this is the moment to take its hash
   br_sha256_context sofar = transcript;
   br_sha256_out(&sofar, sessionHash);
   if (deriveKeys() != 0) return -1;

   if (appendHandshake(HANDSHAKE_CERTIFICATE_VERIFY, writeCertificateVerify) != 0) return -1;
   if (appendChangeCipherSpec() != 0) return -1;
   if (appendFinished() != 0) return -1;

   logInfo(TAG "dtls: sent our half of the exchange, %d bytes\n", flightLength);
   return sendFlight();
}

// section: reading the other side

static const char *getHandshakeName(int type)
{
   switch (type) {
   case HANDSHAKE_SERVER_HELLO:         return "server hello";
   case HANDSHAKE_HELLO_VERIFY_REQUEST: return "hello verify request";
   case HANDSHAKE_CERTIFICATE:          return "certificate";
   case HANDSHAKE_SERVER_KEY_EXCHANGE:  return "key exchange";
   case HANDSHAKE_CERTIFICATE_REQUEST:  return "certificate request";
   case HANDSHAKE_SERVER_HELLO_DONE:    return "hello done";
   case HANDSHAKE_FINISHED:             return "finished";
   }
   return "something else";
}

// the far end can refuse to talk until we prove we are really at the address we claim, by sending
// back a value it chose. the exchange goes round again with that value included. only the hash
// starts over: RFC 6347 4.2.6 leaves the first hello and this refusal out of it, while 4.2.2 keeps
// the message count running, so the second hello is number 1 and the reply to it is number 1 too.
static int takeCookie(const uint8_t *body, int length)
{
   if (length < 3) return -1;
   int offered = body[2];
   if (offered > COOKIE_MAX || 3 + offered > length) {
      logError(TAG "dtls: the cookie it sent does not fit\n");
      return -1;
   }
   memCopy(cookie, body + 3, offered);
   cookieLength = offered;
   logInfo(TAG "dtls: asked to prove our address, %d byte cookie\n", cookieLength);

   br_sha256_init(&transcript);
   return sendClientHello();
}

static int readServerHello(const uint8_t *body, int length)
{
   if (length < 2 + RANDOM_SIZE + 1) return -1;
   memCopy(serverRandom, body + 2, RANDOM_SIZE);

   int at = 2 + RANDOM_SIZE;
   at += 1 + body[at];   // the session it would resume, which we did not ask for
   if (at + 3 > length) return -1;

   logInfo(TAG "dtls: it chose cipher suite 0x%04X\n", read16(body + at));
   at += 3;   // the suite, and the compression it will not be doing

   // what it agreed to out of what we asked for, each one numbered and sized
   if (at + 2 > length) return 0;
   int extensionsEnd = at + 2 + read16(body + at);
   at += 2;
   while (at + 4 <= length && at + 4 <= extensionsEnd) {
      int number = read16(body + at);
      int size = read16(body + at + 2);
      const uint8_t *value = body + at + 4;
      if (at + 4 + size > length) break;
      at += 4 + size;

      if (number == EXTENSION_EXTENDED_MASTER_SECRET) extendedMasterSecret = 1;
      if (number == EXTENSION_USE_SRTP && size >= 4) srtpProfile = read16(value + 2);
   }

   logInfo(TAG "dtls: secret tied to this conversation %s, media cipher 0x%04X\n",
           extendedMasterSecret ? "yes" : "no", srtpProfile);

   // the media keys below are derived for one cipher only, so a different choice here would leave
   // us encrypting the picture in a way the far end cannot read
   if (srtpProfile != SRTP_AES128_CM_HMAC_SHA1_80) {
      logError(TAG "dtls: it wants a media cipher we do not have\n");
      return -1;
   }
   return 0;
}

// Nothing signed the far end's certificate, and no authority vouches for it. What makes it mean
// anything is that the description, which reached us over a connection we already trusted, said in
// advance what its fingerprint would be. So this comparison is the whole of the authentication:
// without it, anything that could reach this socket could take the machine's place.
static int checkServerCertificate(const uint8_t *body, int length)
{
   if (length < 6) return -1;

   // a list of certificates, each with its own length in front of it. the first is the far end's.
   int first = (int)read24(body + 3);
   if (first <= 0 || 6 + first > length) {
      logError(TAG "dtls: its certificate does not fit what it sent\n");
      return -1;
   }

   char seen[DTLS_FINGERPRINT_MAX];
   writeCertificateFingerprint(body + 6, first, seen);

   if (strCmpICase(seen, expectedFingerprint) != 0) {
      logError(TAG "dtls: the machine is not the one the description named\n");
      logError(TAG "dtls: expected %s\n", expectedFingerprint);
      logError(TAG "dtls: received %s\n", seen);
      return -1;
   }

   logInfo(TAG "dtls: the machine matches the fingerprint we were promised\n");
   return 0;
}

// the far end's half of the shared secret, followed by a signature we do not check: the
// fingerprint above is what vouches for this connection, not any authority.
static int readServerKeyExchange(const uint8_t *body, int length)
{
   if (length < 4 || body[0] != 3) {
      logError(TAG "dtls: it offered a key on something other than a named curve\n");
      return -1;
   }
   if (read16(body + 1) != GROUP_SECP256R1) {
      logError(TAG "dtls: it chose curve 0x%04X, which we cannot do\n", read16(body + 1));
      return -1;
   }

   serverPointLength = body[3];
   if (serverPointLength > POINT_MAX || 4 + serverPointLength > length) {
      logError(TAG "dtls: its key does not fit\n");
      return -1;
   }
   memCopy(serverPoint, body + 4, serverPointLength);
   return 0;
}

// the far end's last message, and the proof that it reached the same secret we did. the value is
// derived the same way ours was, over a conversation that now includes ours.
static int checkServerFinished(const uint8_t *body, int length)
{
   if (length != VERIFY_SIZE) {
      logError(TAG "dtls: its closing message was the wrong size\n");
      return -1;
   }

   // the hash here must not include the message being checked, so it is taken before the caller
   // folds it in. that ordering is why this reads expectedTranscript rather than transcript.
   uint8_t digest[br_sha256_SIZE];
   br_sha256_context sofar = expectedTranscript;
   br_sha256_out(&sofar, digest);

   uint8_t expected[VERIFY_SIZE];
   stretch(expected, VERIFY_SIZE, masterSecret, MASTER_SIZE, "server finished", digest, sizeof digest, 0, 0);

   // every byte is compared whether or not an earlier one differed, so how long this takes says
   // nothing about how much of the value was right
   uint8_t difference = 0;
   for (int byte = 0; byte < VERIFY_SIZE; byte++) difference |= (uint8_t)(expected[byte] ^ body[byte]);
   if (difference != 0) {
      logError(TAG "dtls: it did not arrive at the same secret we did\n");
      return -1;
   }

   logInfo(TAG "dtls: encrypted, both sides agree\n");
   return 0;
}

static DtlsProgress readHandshakeMessage(int type, const uint8_t *body, int length)
{
   switch (type) {
   case HANDSHAKE_HELLO_VERIFY_REQUEST:
      return takeCookie(body, length) == 0 ? DTLS_HANDSHAKE_RUNNING : DTLS_HANDSHAKE_FAILED;
   case HANDSHAKE_SERVER_HELLO:
      if (readServerHello(body, length) != 0) return DTLS_HANDSHAKE_FAILED;
      break;
   case HANDSHAKE_CERTIFICATE:
      if (checkServerCertificate(body, length) != 0) return DTLS_HANDSHAKE_FAILED;
      break;
   case HANDSHAKE_SERVER_KEY_EXCHANGE:
      if (readServerKeyExchange(body, length) != 0) return DTLS_HANDSHAKE_FAILED;
      break;
   case HANDSHAKE_SERVER_HELLO_DONE:
      return sendOurFlight() == 0 ? DTLS_HANDSHAKE_RUNNING : DTLS_HANDSHAKE_FAILED;
   case HANDSHAKE_FINISHED:
      return checkServerFinished(body, length) == 0 ? DTLS_HANDSHAKE_DONE : DTLS_HANDSHAKE_FAILED;
   }
   return DTLS_HANDSHAKE_RUNNING;
}

// Once the far end starts encrypting, a record carries the count it used in front of the content
// and a tag behind it, and neither is part of what was said. Writes the content into plain and
// returns its length, or -1 if the tag says the record was altered on the way.
static int decryptRecord(const uint8_t *payload, int recordLength, int type, uint8_t *plain)
{
   int contentLength = recordLength - NONCE_SIZE - TAG_SIZE;
   if (contentLength < 0 || contentLength > FLIGHT_MAX) {
      logError(TAG "dtls: an encrypted record was the wrong size\n");
      return -1;
   }

   memCopy(plain, payload + NONCE_SIZE, contentLength);

   uint8_t associated[RECORD_HEADER];
   buildAssociatedData(associated, payload, type, contentLength);

   uint32_t tagMatched = 0;
   runGcm(readKey, readSalt, payload, associated, 0, plain, contentLength,
          (uint8_t *)(payload + NONCE_SIZE + contentLength), &tagMatched);
   if (!tagMatched) {
      logError(TAG "dtls: a record it encrypted did not survive the check\n");
      return -1;
   }
   return contentLength;
}

// section: the API

// a fresh key for this connection only, so a recording of it cannot be unpicked later
static int makeEphemeralKey(void)
{
   uint8_t seed[32];
   if (sys_get_random_number(seed, sizeof seed) != 0) return -1;

   br_hmac_drbg_context generator;
   br_hmac_drbg_init(&generator, &br_sha256_vtable, seed, sizeof seed);

   const br_ec_impl *curve = br_ec_get_default();
   if (br_ec_keygen(&generator.vtable, curve, &ephemeralKey, ephemeralKeyBuffer, BR_EC_secp256r1) == 0) return -1;

   br_ec_public_key publicKey;
   if (br_ec_compute_pub(curve, &publicKey, ourPoint, &ephemeralKey) == 0) return -1;
   ourPointLength = (int)publicKey.qlen;
   return 0;
}

int startDtls(DtlsSendFunc send, const char *peerFingerprint)
{
   if (!send || !peerFingerprint || !peerFingerprint[0]) return -1;
   sendDatagram = send;
   strCopy(expectedFingerprint, sizeof expectedFingerprint, peerFingerprint);

   if (sys_get_random_number(clientRandom, sizeof clientRandom) != 0) {
      logError(TAG "dtls: the console would not give us randomness\n");
      return -1;
   }
   if (makeEphemeralKey() != 0) {
      logError(TAG "dtls: could not make a key for this connection\n");
      return -1;
   }

   cookieLength = 0;
   serverPointLength = 0;
   messageSequence = 0;
   expectedSequence = 0;
   recordSequence = 0;
   encryptedSequence = 0;
   extendedMasterSecret = 0;
   srtpProfile = 0;
   flightLength = 0;
   br_sha256_init(&transcript);

   return sendClientHello();
}

DtlsProgress feedDtls(const void *datagram, int length)
{
   const uint8_t *data = (const uint8_t *)datagram;
   uint8_t plain[FLIGHT_MAX];
   int at = 0;

   // one datagram can carry several records, each saying how long it is
   while (at + RECORD_HEADER <= length) {
      int type = data[at];
      int epoch = read16(data + at + 3);
      int recordLength = read16(data + at + 11);
      const uint8_t *payload = data + at + RECORD_HEADER;
      if (at + RECORD_HEADER + recordLength > length) {
         logWarn(TAG "dtls: a record claimed more bytes than arrived\n");
         return DTLS_HANDSHAKE_FAILED;
      }
      at += RECORD_HEADER + recordLength;

      if (type == RECORD_ALERT && epoch == 0) {
         logError(TAG "dtls: it refused, alert level %d description %d\n", payload[0], payload[1]);
         return DTLS_HANDSHAKE_FAILED;
      }

      if (epoch != 0) {
         recordLength = decryptRecord(payload, recordLength, type, plain);
         if (recordLength < 0) return DTLS_HANDSHAKE_FAILED;
         payload = plain;
      }

      if (type != RECORD_HANDSHAKE) continue;

      // and one record can carry several messages, each also saying how long it is
      int inRecord = 0;
      while (inRecord + HANDSHAKE_HEADER <= recordLength) {
         const uint8_t *message = payload + inRecord;
         int messageType = message[0];
         uint16_t sequence = read16(message + 4);
         int fragmentLength = (int)read24(message + 9);
         if (inRecord + HANDSHAKE_HEADER + fragmentLength > recordLength) {
            logWarn(TAG "dtls: a message claimed more bytes than its record held\n");
            return DTLS_HANDSHAKE_FAILED;
         }
         inRecord += HANDSHAKE_HEADER + fragmentLength;

         // the far end repeats its whole flight until answered. a repeat must not reach the hash.
         // a message from further ahead than the next one is dropped too, and that is worth a line:
         // it means our count and theirs have come apart, which otherwise only shows as a timeout.
         if (sequence != expectedSequence) {
            if (sequence > expectedSequence)
               logWarn(TAG "dtls: %s is message %d, we are waiting for %d\n", getHandshakeName(messageType), sequence, expectedSequence);
            continue;
         }
         expectedSequence++;

         logInfo(TAG "dtls: received %s, %d bytes\n", getHandshakeName(messageType), fragmentLength);

         // the closing message is checked against the conversation without itself in it, so the
         // hash as it stands is kept before this message joins it
         expectedTranscript = transcript;
         br_sha256_update(&transcript, message, HANDSHAKE_HEADER + fragmentLength);

         DtlsProgress progress = readHandshakeMessage(messageType, message + HANDSHAKE_HEADER, fragmentLength);
         if (progress != DTLS_HANDSHAKE_RUNNING) return progress;
      }
   }
   return DTLS_HANDSHAKE_RUNNING;
}

void resendDtls(void)
{
   if (flightLength > 0) sendDatagram(flight, flightLength);
}

const uint8_t *getDtlsMediaKeys(void) { return srtpKeyBlock; }

int readDtlsData(const void *datagram, int length, uint8_t *out, int capacity)
{
   const uint8_t *data = (const uint8_t *)datagram;
   if (length < RECORD_HEADER) return -1;

   int type = data[0];
   int recordLength = read16(data + 11);
   if (RECORD_HEADER + recordLength > length) return -1;
   if (read16(data + 3) == 0) return 0;   // still unencrypted, so not content for the caller

   uint8_t plain[FLIGHT_MAX];
   int plainLength = decryptRecord(data + RECORD_HEADER, recordLength, type, plain);
   if (plainLength < 0) return -1;

   if (type == RECORD_ALERT) {
      logInfo(TAG "dtls: it closed the connection, alert level %d description %d\n", plain[0], plain[1]);
      return -1;
   }
   if (type != RECORD_APPLICATION_DATA) return 0;

   if (plainLength > capacity) {
      logError(TAG "dtls: it sent more than the caller can hold\n");
      return -1;
   }
   memCopy(out, plain, plainLength);
   return plainLength;
}

int sendDtlsData(const void *data, int length)
{
   uint8_t record[FLIGHT_MAX];
   if (RECORD_HEADER + NONCE_SIZE + length + TAG_SIZE > FLIGHT_MAX) return -1;

   uint8_t sequence[NONCE_SIZE];
   write16(sequence, 0, 1);   // epoch 1: encrypted
   write24(sequence, 2, (uint32_t)(encryptedSequence >> 24));
   write24(sequence, 5, (uint32_t)(encryptedSequence & 0xFFFFFF));

   int at = writeByte(record, 0, RECORD_APPLICATION_DATA);
   at = write16(record, at, DTLS_1_2);
   at = writeBytes(record, at, sequence, NONCE_SIZE);
   at = write16(record, at, (uint16_t)(NONCE_SIZE + length + TAG_SIZE));
   at = writeBytes(record, at, sequence, NONCE_SIZE);

   int contentAt = at;
   at = writeBytes(record, at, data, length);

   uint8_t associated[RECORD_HEADER];
   buildAssociatedData(associated, sequence, RECORD_APPLICATION_DATA, length);
   runGcm(writeKey, writeSalt, sequence, associated, 1, record + contentAt, length, record + at, 0);
   at += TAG_SIZE;

   encryptedSequence++;
   return sendDatagram(record, at);
}
