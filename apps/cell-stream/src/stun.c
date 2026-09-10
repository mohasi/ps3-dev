// stun - see stun.h.
//
// Two fields at the end of a message are unusual and easy to get wrong, so they are spelled out
// here. Both are computed over the message as it will be read, which means the length field has to
// be written ahead of the field being added, counting bytes that are not there yet.

#include "stun.h"

#include "dbg.h"
#include "hmac-sha1.h"
#include "string-utilities.h"

#include <sys/random_number.h>

#define TAG "[cst] "

#define BINDING_REQUEST  0x0001
#define BINDING_SUCCESS  0x0101
#define BINDING_ERROR    0x0111

#define MAGIC_COOKIE 0x2112A442u

#define ATTR_USERNAME          0x0006
#define ATTR_MESSAGE_INTEGRITY 0x0008
#define ATTR_XOR_MAPPED_ADDR   0x0020
#define ATTR_PRIORITY          0x0024
#define ATTR_ICE_CONTROLLING   0x802A
#define ATTR_FINGERPRINT       0x8028
#define ATTR_ERROR_CODE        0x0009
#define ATTR_USE_CANDIDATE     0x0025

#define HEADER_LENGTH    20
#define INTEGRITY_LENGTH 24   // the 4-byte attribute header plus a 20-byte hash
#define FINGERPRINT_LENGTH 8

#define FINGERPRINT_XOR 0x5354554Eu

#define ADDRESS_FAMILY_IPV4 0x01

static void writeUint16(uint8_t *at, uint16_t value)
{
   at[0] = (uint8_t)(value >> 8);
   at[1] = (uint8_t)value;
}

static void writeUint32(uint8_t *at, uint32_t value)
{
   at[0] = (uint8_t)(value >> 24);
   at[1] = (uint8_t)(value >> 16);
   at[2] = (uint8_t)(value >> 8);
   at[3] = (uint8_t)value;
}

static uint16_t readUint16(const uint8_t *at) { return (uint16_t)((at[0] << 8) | at[1]); }
static uint32_t readUint32(const uint8_t *at)
{
   return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

// the ordinary CRC-32, computed a bit at a time: this runs over a few hundred bytes a second, so a
// lookup table would cost more memory than the time it saves
static uint32_t crc32(const uint8_t *data, int length)
{
   uint32_t remainder = 0xFFFFFFFFu;
   for (int at = 0; at < length; at++) {
      remainder ^= data[at];
      for (int bit = 0; bit < 8; bit++)
         remainder = (remainder >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(remainder & 1));
   }
   return remainder ^ 0xFFFFFFFFu;
}

// every attribute is padded out to a multiple of four bytes
static int appendAttribute(uint8_t *out, int at, int capacity, uint16_t type, const void *value, int valueLength)
{
   int padded = (valueLength + 3) & ~3;
   if (at + 4 + padded > capacity) return -1;

   writeUint16(out + at, type);
   writeUint16(out + at + 2, (uint16_t)valueLength);
   memCopy(out + at + 4, value, valueLength);
   for (int pad = valueLength; pad < padded; pad++) out[at + 4 + pad] = 0;
   return at + 4 + padded;
}

// the two fields that close a message. both are computed over the message as it will be read, so
// the length field is written ahead of each one, counting bytes that are not there yet.
static int appendIntegrityAndFingerprint(uint8_t *out, int at, int capacity, const char *password)
{
   if (at + INTEGRITY_LENGTH + FINGERPRINT_LENGTH > capacity) return -1;
   writeUint16(out + 2, (uint16_t)(at + INTEGRITY_LENGTH - HEADER_LENGTH));

   uint8_t hash[20];
   hmacSha1((const uint8_t *)password, getStrLen(password), out, at, hash);
   at = appendAttribute(out, at, capacity, ATTR_MESSAGE_INTEGRITY, hash, sizeof hash);
   if (at < 0) return -1;

   writeUint16(out + 2, (uint16_t)(at + FINGERPRINT_LENGTH - HEADER_LENGTH));

   uint8_t fingerprintBytes[4];
   writeUint32(fingerprintBytes, crc32(out, at) ^ FINGERPRINT_XOR);
   return appendAttribute(out, at, capacity, ATTR_FINGERPRINT, fingerprintBytes, sizeof fingerprintBytes);
}

int buildStunRequest(uint8_t *out, int capacity, const char *username, const char *password, uint32_t priority,
                     uint64_t tiebreaker, int nominate, StunPendingRequest *request)
{
   if (capacity < HEADER_LENGTH) return -1;
   if (sys_get_random_number(request->transaction, STUN_TRANSACTION_LENGTH) != 0) return -1;

   writeUint16(out, BINDING_REQUEST);
   writeUint16(out + 2, 0);   // filled in below, twice
   writeUint32(out + 4, MAGIC_COOKIE);
   memCopy(out + 8, request->transaction, STUN_TRANSACTION_LENGTH);

   int at = HEADER_LENGTH;

   at = appendAttribute(out, at, capacity, ATTR_USERNAME, username, getStrLen(username));
   if (at < 0) return -1;

   uint8_t priorityBytes[4];
   writeUint32(priorityBytes, priority);
   at = appendAttribute(out, at, capacity, ATTR_PRIORITY, priorityBytes, sizeof priorityBytes);
   if (at < 0) return -1;

   uint8_t tiebreakerBytes[8];
   writeUint32(tiebreakerBytes, (uint32_t)(tiebreaker >> 32));
   writeUint32(tiebreakerBytes + 4, (uint32_t)tiebreaker);
   at = appendAttribute(out, at, capacity, ATTR_ICE_CONTROLLING, tiebreakerBytes, sizeof tiebreakerBytes);
   if (at < 0) return -1;

   // this one carries no value; its presence is the whole message
   if (nominate) {
      at = appendAttribute(out, at, capacity, ATTR_USE_CANDIDATE, NULL, 0);
      if (at < 0) return -1;
   }

   return appendIntegrityAndFingerprint(out, at, capacity, password);
}

StunReplyKind readStunReply(const uint8_t *message, int length, const StunPendingRequest *request,
                            uint32_t *seenAddress, uint16_t *seenPort, StunError *error)
{
   if (error) { error->code = 0; error->reason[0] = 0; }

   if (length < HEADER_LENGTH) return STUN_REPLY_NOT_STUN;
   if ((message[0] & 0xC0) != 0) return STUN_REPLY_NOT_STUN;   // the top two bits are always zero
   if (readUint32(message + 4) != MAGIC_COOKIE) return STUN_REPLY_NOT_STUN;
   if (HEADER_LENGTH + readUint16(message + 2) != length) return STUN_REPLY_NOT_STUN;

   for (int at = 0; at < STUN_TRANSACTION_LENGTH; at++)
      if (message[8 + at] != request->transaction[at]) return STUN_REPLY_OTHER;

   uint16_t type = readUint16(message);
   if (type != BINDING_SUCCESS && type != BINDING_ERROR) return STUN_REPLY_OTHER;

   // the address the far end saw us on is obscured by constants both ends already know; a refusal
   // instead carries a code and a few words saying what was wrong with the message
   for (int at = HEADER_LENGTH; at + 4 <= length;) {
      uint16_t attribute = readUint16(message + at);
      int valueLength = readUint16(message + at + 2);
      int valueAt = at + 4;
      if (valueAt + valueLength > length) break;

      if (attribute == ATTR_XOR_MAPPED_ADDR && valueLength >= 8 && message[valueAt + 1] == ADDRESS_FAMILY_IPV4) {
         if (seenPort) *seenPort = (uint16_t)(readUint16(message + valueAt + 2) ^ (MAGIC_COOKIE >> 16));
         if (seenAddress) *seenAddress = readUint32(message + valueAt + 4) ^ MAGIC_COOKIE;
      }

      if (attribute == ATTR_ERROR_CODE && valueLength >= 4 && error) {
         error->code = (message[valueAt + 2] & 7) * 100 + message[valueAt + 3];
         int reasonLength = valueLength - 4;
         if (reasonLength > (int)sizeof error->reason - 1) reasonLength = (int)sizeof error->reason - 1;
         memCopy(error->reason, message + valueAt + 4, reasonLength);
         error->reason[reasonLength] = 0;
      }

      at = valueAt + ((valueLength + 3) & ~3);
   }

   return type == BINDING_SUCCESS ? STUN_REPLY_SUCCESS : STUN_REPLY_ERROR;
}

// section: answering a check the far end sent us

int readStunRequest(const uint8_t *message, int length, StunIncomingRequest *request)
{
   if (length < HEADER_LENGTH) return -1;
   if ((message[0] & 0xC0) != 0) return -1;
   if (readUint32(message + 4) != MAGIC_COOKIE) return -1;
   if (HEADER_LENGTH + readUint16(message + 2) != length) return -1;
   if (readUint16(message) != BINDING_REQUEST) return -1;

   memCopy(request->transaction, message + 8, STUN_TRANSACTION_LENGTH);
   request->username[0] = 0;
   request->nominated = 0;

   for (int at = HEADER_LENGTH; at + 4 <= length;) {
      uint16_t attribute = readUint16(message + at);
      int valueLength = readUint16(message + at + 2);
      int valueAt = at + 4;
      if (valueAt + valueLength > length) break;

      if (attribute == ATTR_USERNAME) {
         int take = valueLength < STUN_USERNAME_MAX - 1 ? valueLength : STUN_USERNAME_MAX - 1;
         memCopy(request->username, message + valueAt, take);
         request->username[take] = 0;
      }
      if (attribute == ATTR_USE_CANDIDATE) request->nominated = 1;

      at = valueAt + ((valueLength + 3) & ~3);
   }

   return 0;
}

int buildStunResponse(uint8_t *out, int capacity, const StunIncomingRequest *request, const char *password,
                      uint32_t theirAddress, uint16_t theirPort)
{
   if (capacity < HEADER_LENGTH) return -1;

   writeUint16(out, BINDING_SUCCESS);
   writeUint16(out + 2, 0);   // filled in by the closing fields below
   writeUint32(out + 4, MAGIC_COOKIE);
   memCopy(out + 8, request->transaction, STUN_TRANSACTION_LENGTH);

   // where we received it from, obscured the same way the far end obscures ours
   uint8_t address[8];
   address[0] = 0;
   address[1] = ADDRESS_FAMILY_IPV4;
   writeUint16(address + 2, (uint16_t)(theirPort ^ (MAGIC_COOKIE >> 16)));
   writeUint32(address + 4, theirAddress ^ MAGIC_COOKIE);

   int at = appendAttribute(out, HEADER_LENGTH, capacity, ATTR_XOR_MAPPED_ADDR, address, sizeof address);
   if (at < 0) return -1;

   return appendIntegrityAndFingerprint(out, at, capacity, password);
}

// section: self test

// Every expected value below is copied from the document named beside it. Nothing here is derived
// from our own output, which is the point: a check that only agrees with itself proves nothing.

// RFC 2202 test case 1
static const uint8_t HMAC_KEY_ONE[20] = {
   0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
   0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b
};
static const char HMAC_DATA_ONE[] = "Hi There";
static const uint8_t HMAC_EXPECTED_ONE[20] = {
   0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
   0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00
};

// RFC 2202 test case 2
static const char HMAC_KEY_TWO[] = "Jefe";
static const char HMAC_DATA_TWO[] = "what do ya want for nothing?";
static const uint8_t HMAC_EXPECTED_TWO[20] = {
   0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
   0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79
};

// the check value every CRC-32 implementation is expected to produce, from the standard's own
// description of the algorithm
static const char CRC_DATA[] = "123456789";
#define CRC_EXPECTED 0xCBF43926u

static int checkHash(const char *name, const uint8_t *key, int keyLength, const char *data,
                     const uint8_t expected[20])
{
   uint8_t got[20];
   hmacSha1(key, keyLength, (const uint8_t *)data, getStrLen(data), got);
   for (int at = 0; at < 20; at++)
      if (got[at] != expected[at]) {
         logError(TAG "%s FAILED at byte %d, got 0x%02x expected 0x%02x\n", name, at, got[at], expected[at]);
         return 1;
      }
   return 0;
}

int runStunSelfTest(void)
{
   int failures = 0;

   failures += checkHash("hmac-sha1 case 1", HMAC_KEY_ONE, sizeof HMAC_KEY_ONE, HMAC_DATA_ONE, HMAC_EXPECTED_ONE);
   failures += checkHash("hmac-sha1 case 2", (const uint8_t *)HMAC_KEY_TWO, getStrLen(HMAC_KEY_TWO), HMAC_DATA_TWO,
                         HMAC_EXPECTED_TWO);

   uint32_t crc = crc32((const uint8_t *)CRC_DATA, getStrLen(CRC_DATA));
   if (crc != CRC_EXPECTED) {
      logError(TAG "crc32 FAILED, got 0x%08x expected 0x%08x\n", crc, CRC_EXPECTED);
      failures++;
   }

   logInfo(TAG "stun self test: %d failures\n", failures);
   return failures;
}
