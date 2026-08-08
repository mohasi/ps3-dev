#include "wg-handshake.h"

#include <sys/sys_time.h>

#include "blake2s.h"
#include "chacha20-poly1305.h"
#include "dbg.h"
#include "string-utilities.h"
#include "wg-bytes.h"
#include "wg-random.h"
#include "x25519.h"

#define TAG "[wg] "

// the four fixed strings of the protocol, whitepaper section 5.4
static const char CONSTRUCTION[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
static const char IDENTIFIER[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";
static const char LABEL_MAC1[] = "mac1----";

// field offsets in the initiation message
#define INITIATION_SENDER_INDEX 4
#define INITIATION_EPHEMERAL    8
#define INITIATION_STATIC       40   // 32 bytes of key plus a 16 byte tag
#define INITIATION_TIMESTAMP    88   // 12 bytes of time plus a 16 byte tag
#define INITIATION_MAC1         116
#define INITIATION_MAC2         132

// field offsets in the response message
#define RESPONSE_SENDER_INDEX   4
#define RESPONSE_RECEIVER_INDEX 8
#define RESPONSE_EPHEMERAL      12
#define RESPONSE_EMPTY          44   // an empty payload, so just the 16 byte tag
#define RESPONSE_MAC1           60

// hash = BLAKE2s(hash || data)
static void mixHash(uint8_t hash[32], const void *data, int length)
{
   Blake2sState state;
   initBlake2s(&state, 32);
   updateBlake2s(&state, hash, 32);
   updateBlake2s(&state, data, length);
   finishBlake2s(&state, hash);
}

// HKDF over HMAC-BLAKE2s. outputs two and three are optional; pass NULL for the ones not wanted.
static void deriveKeys(uint8_t *first, uint8_t *second, uint8_t *third, const uint8_t chainKey[32],
                       const void *input, int inputLength)
{
   uint8_t secret[32];
   hmacBlake2s(secret, chainKey, 32, input, inputLength);

   uint8_t counter = 1;
   hmacBlake2s(first, secret, 32, &counter, 1);

   if (second) {
      uint8_t chained[33];
      memCopy(chained, first, 32);
      chained[32] = 2;
      hmacBlake2s(second, secret, 32, chained, sizeof chained);

      if (third) {
         memCopy(chained, second, 32);
         chained[32] = 3;
         hmacBlake2s(third, secret, 32, chained, sizeof chained);
      }
      memSet(chained, 0, sizeof chained);
   }

   memSet(secret, 0, sizeof secret);
}

// One Diffie-Hellman step: combine the two keys, fold the result into the chaining key, and
// optionally take an encryption key out of it. stepName names the step in the failure line, so a
// zero result says which of the four exchanges produced it.
static int mixDiffieHellman(WgHandshake *handshake, const uint8_t privateKey[32], const uint8_t publicKey[32],
                            uint8_t *keyOut, const char *stepName)
{
   uint8_t shared[32];
   if (!computeX25519Shared(shared, privateKey, publicKey)) {
      logError(TAG "handshake: %s produced an unusable shared secret\n", stepName);
      return -1;
   }

   deriveKeys(handshake->chainKey, keyOut, 0, handshake->chainKey, shared, 32);
   memSet(shared, 0, sizeof shared);
   return 0;
}

// TAI64N: seconds since the TAI epoch as 8 big-endian bytes, then nanoseconds as 4 more.
// the server only requires this to increase between handshakes from the same peer.
static void getTai64nTimestamp(uint8_t timestamp[12])
{
   sys_time_sec_t seconds = 0;
   sys_time_nsec_t nanoseconds = 0;
   sys_time_get_current_time(&seconds, &nanoseconds);

   uint64_t tai = 0x400000000000000AULL + (uint64_t)seconds;
   for (int index = 0; index < 8; index++) timestamp[index] = (uint8_t)(tai >> (56 - index * 8));

   uint32_t fraction = (uint32_t)nanoseconds;
   for (int index = 0; index < 4; index++) timestamp[8 + index] = (uint8_t)(fraction >> (24 - index * 8));
}

// mac1 authenticates the message with a key derived from the receiver's public key, proving the
// sender knows who it is talking to. it covers everything before the mac1 field itself.
void computeWgMac1(uint8_t mac[16], const uint8_t receiverPublicKey[32], const uint8_t *message, int length)
{
   uint8_t macKey[32];
   Blake2sState state;
   initBlake2s(&state, 32);
   updateBlake2s(&state, LABEL_MAC1, (int)(sizeof LABEL_MAC1 - 1));
   updateBlake2s(&state, receiverPublicKey, 32);
   finishBlake2s(&state, macKey);

   macBlake2s(mac, 16, macKey, 32, message, length);
   memSet(macKey, 0, sizeof macKey);
}

// a fresh key pair and index for this handshake, whichever side is starting it
static int generateEphemeralKeys(WgHandshake *handshake)
{
   uint8_t indexBytes[4];
   if (getRandomBytes(handshake->ephemeralPrivate, 32) != 0) return -1;
   if (getRandomBytes(indexBytes, sizeof indexBytes) != 0) return -1;

   getX25519PublicKey(handshake->ephemeralPublic, handshake->ephemeralPrivate);
   handshake->senderIndex = load32le(indexBytes);
   return 0;
}

// the fixed opening state of every handshake, keyed by whoever receives the first message
static void beginHandshakeState(WgHandshake *handshake, const uint8_t receiverPublicKey[32])
{
   hashBlake2s(handshake->chainKey, 32, CONSTRUCTION, (int)(sizeof CONSTRUCTION - 1));

   Blake2sState state;
   initBlake2s(&state, 32);
   updateBlake2s(&state, handshake->chainKey, 32);
   updateBlake2s(&state, IDENTIFIER, (int)(sizeof IDENTIFIER - 1));
   finishBlake2s(&state, handshake->hash);

   mixHash(handshake->hash, receiverPublicKey, 32);
}

int createWgInitiation(WgHandshake *handshake, const WgConfig *config, uint8_t message[WG_INITIATION_LENGTH])
{
   memSet(handshake, 0, sizeof *handshake);
   memCopy(handshake->staticPrivate, config->privateKey, 32);
   memCopy(handshake->peerPublicKey, config->peerPublicKey, 32);
   if (config->hasPresharedKey) memCopy(handshake->presharedKey, config->presharedKey, 32);
   getX25519PublicKey(handshake->staticPublic, handshake->staticPrivate);

   // the server is the receiver of this message, so the opening state is keyed by its public key
   beginHandshakeState(handshake, handshake->peerPublicKey);

   if (generateEphemeralKeys(handshake) != 0) return -1;

   memSet(message, 0, WG_INITIATION_LENGTH);
   message[0] = WG_MESSAGE_INITIATION;
   store32le(message + INITIATION_SENDER_INDEX, handshake->senderIndex);
   memCopy(message + INITIATION_EPHEMERAL, handshake->ephemeralPublic, 32);

   deriveKeys(handshake->chainKey, 0, 0, handshake->chainKey, handshake->ephemeralPublic, 32);
   mixHash(handshake->hash, handshake->ephemeralPublic, 32);

   // our identity, encrypted so only the server can read it
   uint8_t key[32], nonce[AEAD_NONCE_LENGTH];
   if (mixDiffieHellman(handshake, handshake->ephemeralPrivate, handshake->peerPublicKey, key, "ephemeral to peer") != 0)
      return -1;
   storeWgNonce(nonce, 0);
   sealChaCha20Poly1305(message + INITIATION_STATIC, handshake->staticPublic, 32, handshake->hash, 32, nonce, key);
   mixHash(handshake->hash, message + INITIATION_STATIC, 48);

   // the timestamp, encrypted under a key that proves we hold our own private key
   if (mixDiffieHellman(handshake, handshake->staticPrivate, handshake->peerPublicKey, key, "static to peer") != 0)
      return -1;

   uint8_t timestamp[12];
   getTai64nTimestamp(timestamp);
   storeWgNonce(nonce, 0);
   sealChaCha20Poly1305(message + INITIATION_TIMESTAMP, timestamp, sizeof timestamp, handshake->hash, 32, nonce, key);
   mixHash(handshake->hash, message + INITIATION_TIMESTAMP, 28);

   // mac1 over everything so far; mac2 stays zero until the server asks for a cookie
   computeWgMac1(message + INITIATION_MAC1, handshake->peerPublicKey, message, INITIATION_MAC1);
   memSet(message + INITIATION_MAC2, 0, 16);

   memSet(key, 0, sizeof key);
   return 0;
}

int processWgInitiation(WgHandshake *handshake, const WgConfig *config, const uint8_t *message, int length,
                        uint8_t lastTimestamp[12])
{
   if (length != WG_INITIATION_LENGTH) {
      logError(TAG "initiation: wrong length, %d bytes not %d\n", length, WG_INITIATION_LENGTH);
      return -1;
   }

   memSet(handshake, 0, sizeof *handshake);
   memCopy(handshake->staticPrivate, config->privateKey, 32);
   memCopy(handshake->peerPublicKey, config->peerPublicKey, 32);
   if (config->hasPresharedKey) memCopy(handshake->presharedKey, config->presharedKey, 32);
   getX25519PublicKey(handshake->staticPublic, handshake->staticPrivate);

   // the authenticator is keyed by our own public key, so this proves it was aimed at us
   uint8_t expectedMac[16];
   computeWgMac1(expectedMac, handshake->staticPublic, message, INITIATION_MAC1);
   if (!bytesEqual(expectedMac, message + INITIATION_MAC1, 16)) {
      logError(TAG "initiation: authenticator does not match, not meant for us\n");
      return -1;
   }

   // we are the receiver this time, so the opening state is keyed by our public key
   beginHandshakeState(handshake, handshake->staticPublic);
   memCopy(handshake->peerEphemeral, message + INITIATION_EPHEMERAL, 32);
   handshake->receiverIndex = load32le(message + INITIATION_SENDER_INDEX);

   deriveKeys(handshake->chainKey, 0, 0, handshake->chainKey, handshake->peerEphemeral, 32);
   mixHash(handshake->hash, handshake->peerEphemeral, 32);

   // open their identity and check it is the peer our config names
   uint8_t key[32], nonce[AEAD_NONCE_LENGTH], senderStatic[32];
   if (mixDiffieHellman(handshake, handshake->staticPrivate, handshake->peerEphemeral, key, "static to their ephemeral") != 0)
      return -1;

   storeWgNonce(nonce, 0);
   if (!openChaCha20Poly1305(senderStatic, message + INITIATION_STATIC, 48, handshake->hash, 32, nonce, key)) {
      logError(TAG "initiation: sender identity did not open\n");
      return -1;
   }
   if (!bytesEqual(senderStatic, handshake->peerPublicKey, 32)) {
      logError(TAG "initiation: came from a peer our config does not name\n");
      return -1;
   }
   mixHash(handshake->hash, message + INITIATION_STATIC, 48);

   // open the timestamp and refuse anything not newer than the last one, which is what stops an
   // old initiation being recorded and replayed later
   if (mixDiffieHellman(handshake, handshake->staticPrivate, handshake->peerPublicKey, key, "static to their static") != 0)
      return -1;

   uint8_t timestamp[12];
   storeWgNonce(nonce, 0);
   if (!openChaCha20Poly1305(timestamp, message + INITIATION_TIMESTAMP, 28, handshake->hash, 32, nonce, key)) {
      logError(TAG "initiation: timestamp did not open\n");
      return -1;
   }

   int newer = 0;
   for (int index = 0; index < 12 && !newer; index++) {
      if (timestamp[index] > lastTimestamp[index]) newer = 1;
      else if (timestamp[index] < lastTimestamp[index]) break;
   }
   if (!newer) {
      logWarn(TAG "initiation: timestamp is not newer than the last one, ignored as a replay\n");
      return -1;
   }
   memCopy(lastTimestamp, timestamp, 12);
   mixHash(handshake->hash, message + INITIATION_TIMESTAMP, 28);

   memSet(key, 0, sizeof key);
   return 0;
}

int createWgResponse(WgHandshake *handshake, uint8_t message[WG_RESPONSE_LENGTH])
{
   if (generateEphemeralKeys(handshake) != 0) return -1;

   memSet(message, 0, WG_RESPONSE_LENGTH);
   message[0] = WG_MESSAGE_RESPONSE;
   store32le(message + RESPONSE_SENDER_INDEX, handshake->senderIndex);
   store32le(message + RESPONSE_RECEIVER_INDEX, handshake->receiverIndex);
   memCopy(message + RESPONSE_EPHEMERAL, handshake->ephemeralPublic, 32);

   deriveKeys(handshake->chainKey, 0, 0, handshake->chainKey, handshake->ephemeralPublic, 32);
   mixHash(handshake->hash, handshake->ephemeralPublic, 32);

   if (mixDiffieHellman(handshake, handshake->ephemeralPrivate, handshake->peerEphemeral, 0, "ephemeral to their ephemeral") != 0)
      return -1;
   if (mixDiffieHellman(handshake, handshake->ephemeralPrivate, handshake->peerPublicKey, 0, "ephemeral to their static") != 0)
      return -1;

   uint8_t presharedMix[32], key[32], nonce[AEAD_NONCE_LENGTH];
   deriveKeys(handshake->chainKey, presharedMix, key, handshake->chainKey, handshake->presharedKey, 32);
   mixHash(handshake->hash, presharedMix, 32);

   storeWgNonce(nonce, 0);
   sealChaCha20Poly1305(message + RESPONSE_EMPTY, 0, 0, handshake->hash, 32, nonce, key);
   mixHash(handshake->hash, message + RESPONSE_EMPTY, 16);

   // the authenticator on a reply is keyed by the other side's public key
   computeWgMac1(message + RESPONSE_MAC1, handshake->peerPublicKey, message, RESPONSE_MAC1);

   // the two transport keys come out in the initiator's order, so answering a handshake takes
   // them the other way round
   deriveKeys(handshake->receiveKey, handshake->sendKey, 0, handshake->chainKey, 0, 0);

   memSet(presharedMix, 0, sizeof presharedMix);
   memSet(key, 0, sizeof key);
   return 0;
}

int processWgResponse(WgHandshake *handshake, const uint8_t *message, int length)
{
   // each check below reports a different cause, so a failure says what went wrong
   if (length != WG_RESPONSE_LENGTH) {
      logError(TAG "response: wrong length, %d bytes not %d\n", length, WG_RESPONSE_LENGTH);
      return -1;
   }
   if (message[0] == WG_MESSAGE_COOKIE) {
      logError(TAG "response: server sent a cookie reply, so it rejected our authenticator or is under load\n");
      return -1;
   }
   if (message[0] != WG_MESSAGE_RESPONSE) {
      logError(TAG "response: unexpected message type %d\n", message[0]);
      return -1;
   }

   uint32_t receiverIndex = load32le(message + RESPONSE_RECEIVER_INDEX);
   if (receiverIndex != handshake->senderIndex) {
      logError(TAG "response: meant for another handshake, index 0x%x not 0x%x\n", receiverIndex,
               handshake->senderIndex);
      return -1;
   }

   uint8_t expectedMac[16];
   computeWgMac1(expectedMac, handshake->staticPublic, message, RESPONSE_MAC1);
   if (!bytesEqual(expectedMac, message + RESPONSE_MAC1, 16)) {
      logError(TAG "response: authenticator does not match, the reply was not built for us\n");
      return -1;
   }

   handshake->receiverIndex = load32le(message + RESPONSE_SENDER_INDEX);
   const uint8_t *peerEphemeral = message + RESPONSE_EPHEMERAL;

   deriveKeys(handshake->chainKey, 0, 0, handshake->chainKey, peerEphemeral, 32);
   mixHash(handshake->hash, peerEphemeral, 32);

   // both of our keys against their ephemeral, which is what ties the session to both identities
   if (mixDiffieHellman(handshake, handshake->ephemeralPrivate, peerEphemeral, 0, "ephemeral to their ephemeral") != 0)
      return -1;
   if (mixDiffieHellman(handshake, handshake->staticPrivate, peerEphemeral, 0, "static to their ephemeral") != 0)
      return -1;

   uint8_t presharedMix[32], key[32], nonce[AEAD_NONCE_LENGTH];
   deriveKeys(handshake->chainKey, presharedMix, key, handshake->chainKey, handshake->presharedKey, 32);
   mixHash(handshake->hash, presharedMix, 32);

   // the empty payload is the proof: it only opens if every step above matched the server's
   storeWgNonce(nonce, 0);
   if (!openChaCha20Poly1305(0, message + RESPONSE_EMPTY, 16, handshake->hash, 32, nonce, key)) {
      logError(TAG "response: payload did not open, so the derived keys differ from the server's\n");
      return -1;
   }
   mixHash(handshake->hash, message + RESPONSE_EMPTY, 16);

   // transport keys, derived from the finished chaining key with no further input
   deriveKeys(handshake->sendKey, handshake->receiveKey, 0, handshake->chainKey, 0, 0);

   memSet(presharedMix, 0, sizeof presharedMix);
   memSet(key, 0, sizeof key);
   return 0;
}
