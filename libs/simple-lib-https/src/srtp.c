// srtp - see srtp.h.
//
// Keys are worked out from the one the connection agreed: one that hides the contents and one that
// proves the packet was not altered, under different names so no two purposes share a key. The
// picture has its own pair, and the reports another, in each direction.
//
// A media packet carries only the low sixteen bits of its number, so the rest is counted here,
// because the same number must never be used twice with the same key. A report carries its whole
// number, so there is nothing to count.

#include "srtp.h"

#include "dbg.h"
#include "string-utilities.h"

#include "bearssl.h"

#define TAG "[tls] "

#define MASTER_KEY_SIZE  16
#define MASTER_SALT_SIZE 14
#define AUTH_KEY_SIZE    20   // SHA-1 works on twenty bytes at a time
#define TAG_SIZE         10   // the proof is cut to eighty bits, as the agreed cipher says
#define HEADER_SIZE      12   // version, kind, number, time, and who sent it

// what each derived key is called, from RFC 3711. the reports have their own three so that a key
// for the picture can never also protect a report.
#define LABEL_CIPHER 0x00
#define LABEL_AUTH   0x01
#define LABEL_SALT   0x02
#define LABEL_REPORT_CIPHER 0x03
#define LABEL_REPORT_AUTH   0x04
#define LABEL_REPORT_SALT   0x05

#define REPORT_HEADER_SIZE 8   // version and kind, length, and who sent it: the part left readable

#define SENDER_MAX 4   // the picture and the sound, with room to spare

// The three values RFC 3711 derives from one master key. The first two are kept in the form the
// cipher and the signing want them, worked out once here rather than on every packet: media arrives
// more than a thousand times a second and both were being rebuilt each time.
typedef struct {
   br_aes_ct_ctr_keys cipher;
   br_hmac_key_context signing;
   uint8_t salt[MASTER_SALT_SIZE];
} SessionKeys;

static SessionKeys media;                          // the picture and the sound, which only arrive
static SessionKeys outgoingReports, incomingReports;   // each direction's reports, from that end's master key
static uint32_t reportsSent;   // numbers each report, so no two are hidden the same way

// the part of each packet's number that is not written in it
typedef struct {
   uint32_t sender;
   uint32_t rounds;      // how many times the number in the packet has run out and started again
   uint16_t lastNumber;
   int started;   // whether a packet from this sender has ever proved genuine
   int used;
} SenderState;

static SenderState senders[SENDER_MAX];

static uint16_t read16(const uint8_t *data) { return (uint16_t)((data[0] << 8) | data[1]); }

static uint32_t read32(const uint8_t *data)
{
   return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

// The cipher is given the starting block as twelve bytes and a separate four byte count, but the
// block this needs has meaning in all sixteen. So the last four bytes are handed over as that
// count, which is where they belong. Passing zero instead silently uses the wrong block, and the
// keys that come out are wrong in a way nothing later can detect.
static uint32_t getTrailingCount(const uint8_t *start)
{
   return ((uint32_t)start[12] << 24) | ((uint32_t)start[13] << 16) | ((uint32_t)start[14] << 8) | start[15];
}

// Works out one of the keys. All three come from running the cipher over nothing at all, with a
// starting value built from the salt and the key's own name, so a key for one purpose can never
// come out equal to a key for another.
static void deriveKey(const uint8_t *master, const uint8_t *salt, int label, uint8_t *out, int length)
{
   uint8_t start[16];
   memSet(start, 0, sizeof start);
   memCopy(start, salt, MASTER_SALT_SIZE);
   start[7] ^= (uint8_t)label;

   br_aes_ct_ctr_keys cipher;
   br_aes_ct_ctr_init(&cipher, master, MASTER_KEY_SIZE);

   memSet(out, 0, length);
   br_aes_ct_ctr_run(&cipher, start, getTrailingCount(start), out, (size_t)length);
}

// Hides or uncovers a run of bytes, which for this cipher is the same operation. The starting
// value is the salt, who sent the packet and the packet's own number combined, so no two packets
// are ever hidden the same way.
static void runCipher(const SessionKeys *keys, uint32_t sender, uint64_t number,
                      uint8_t *data, int length)
{
   uint8_t start[16];
   memSet(start, 0, sizeof start);
   memCopy(start, keys->salt, MASTER_SALT_SIZE);

   start[4] ^= (uint8_t)(sender >> 24);
   start[5] ^= (uint8_t)(sender >> 16);
   start[6] ^= (uint8_t)(sender >> 8);
   start[7] ^= (uint8_t)sender;

   start[8]  ^= (uint8_t)(number >> 40);
   start[9]  ^= (uint8_t)(number >> 32);
   start[10] ^= (uint8_t)(number >> 24);
   start[11] ^= (uint8_t)(number >> 16);
   start[12] ^= (uint8_t)(number >> 8);
   start[13] ^= (uint8_t)number;

   br_aes_ct_ctr_run(&keys->cipher, start, getTrailingCount(start), data, (size_t)length);
}

// the proof that a packet was not altered, cut to eighty bits. `extra` is folded in after the
// packet itself for media, where the part of the number that is not on the wire has to be covered
// too; reports carry their whole number, so they pass nothing.
static void signPacket(const SessionKeys *keys, const uint8_t *packet, int length, const uint8_t *extra,
                       int extraLength, uint8_t *out)
{
   br_hmac_context hmac;
   br_hmac_init(&hmac, &keys->signing, 0);
   br_hmac_update(&hmac, packet, length);
   if (extraLength) br_hmac_update(&hmac, extra, extraLength);

   uint8_t full[br_sha1_SIZE];
   br_hmac_out(&hmac, full);
   memCopy(out, full, TAG_SIZE);
}

static int openKeys(SessionKeys *keys, const uint8_t *key, const uint8_t *salt,
                    int cipherLabel, int authLabel, int saltLabel)
{
   if (!key || !salt) return -1;

   uint8_t cipherKey[MASTER_KEY_SIZE], authKey[AUTH_KEY_SIZE];
   deriveKey(key, salt, cipherLabel, cipherKey, sizeof cipherKey);
   deriveKey(key, salt, authLabel, authKey, sizeof authKey);
   deriveKey(key, salt, saltLabel, keys->salt, sizeof keys->salt);

   br_aes_ct_ctr_init(&keys->cipher, cipherKey, sizeof cipherKey);
   br_hmac_key_init(&keys->signing, &br_sha1_vtable, authKey, sizeof authKey);
   return 0;
}

int openSrtp(const uint8_t *key, const uint8_t *salt)
{
   if (openKeys(&media, key, salt, LABEL_CIPHER, LABEL_AUTH, LABEL_SALT) != 0) return -1;

   memSet(senders, 0, sizeof senders);
   logInfo(TAG "srtp: ready to unwrap the picture and sound\n");
   return 0;
}

static void runRolloverSelfTest(void);

void runSrtpSelfTest(void)
{
   static const uint8_t MASTER_KEY[MASTER_KEY_SIZE] = {
      0xE1, 0xF9, 0x7A, 0x0D, 0x3E, 0x01, 0x8B, 0xE0, 0xD6, 0x4F, 0xA3, 0x2C, 0x06, 0xDE, 0x41, 0x39
   };
   static const uint8_t MASTER_SALT[MASTER_SALT_SIZE] = {
      0x0E, 0xC6, 0x75, 0xAD, 0x49, 0x8A, 0xFE, 0xEB, 0xB6, 0x96, 0x0B, 0x3A, 0xAB, 0xE6
   };
   static const uint8_t WANTED_CIPHER[MASTER_KEY_SIZE] = {
      0xC6, 0x1E, 0x7A, 0x93, 0x74, 0x4F, 0x39, 0xEE, 0x10, 0x73, 0x4A, 0xFE, 0x3F, 0xF7, 0xA0, 0x87
   };
   static const uint8_t WANTED_SALT[MASTER_SALT_SIZE] = {
      0x30, 0xCB, 0xBC, 0x08, 0x86, 0x3D, 0x8C, 0x85, 0xD4, 0x9D, 0xB3, 0x4A, 0x9A, 0xE1
   };
   static const uint8_t WANTED_AUTH[AUTH_KEY_SIZE] = {
      0xCE, 0xBE, 0x32, 0x1F, 0x6F, 0xF7, 0x71, 0x6B, 0x6F, 0xD4, 0xAB, 0x49, 0xAF, 0x25, 0x6A, 0x15,
      0x6D, 0x38, 0xBA, 0xA4
   };

   uint8_t got[AUTH_KEY_SIZE];
   int failures = 0;

   deriveKey(MASTER_KEY, MASTER_SALT, LABEL_CIPHER, got, MASTER_KEY_SIZE);
   for (int byte = 0; byte < MASTER_KEY_SIZE; byte++) failures += got[byte] != WANTED_CIPHER[byte];

   deriveKey(MASTER_KEY, MASTER_SALT, LABEL_SALT, got, MASTER_SALT_SIZE);
   for (int byte = 0; byte < MASTER_SALT_SIZE; byte++) failures += got[byte] != WANTED_SALT[byte];

   deriveKey(MASTER_KEY, MASTER_SALT, LABEL_AUTH, got, AUTH_KEY_SIZE);
   for (int byte = 0; byte < AUTH_KEY_SIZE; byte++) failures += got[byte] != WANTED_AUTH[byte];

   if (failures) logError(TAG "srtp self test: %d bytes wrong\n", failures);
   else logInfo(TAG "srtp self test: 0 failures\n");

   runRolloverSelfTest();
}


static SenderState *findSender(uint32_t sender)
{
   SenderState *spare = 0;
   for (int index = 0; index < SENDER_MAX; index++) {
      if (senders[index].used && senders[index].sender == sender) return &senders[index];
      if (!senders[index].used && !spare) spare = &senders[index];
   }
   if (!spare) return 0;

   spare->used = 1;
   spare->sender = sender;
   spare->rounds = 0;
   spare->lastNumber = 0;
   spare->started = 0;
   return spare;
}

// Which round a packet belongs to, worked out without changing anything. RFC 3711 Appendix A: the
// answer is a guess, because only sixteen bits of the number are on the wire, and a packet close to
// either side of the point where it runs out could belong to the round before or the round after.
static uint32_t guessRounds(const SenderState *state, uint16_t number)
{
   if (!state->started) return state->rounds;

   int seen = state->lastNumber, arrived = number;
   if (seen < 0x8000) return arrived - seen > 0x8000 ? state->rounds - 1 : state->rounds;
   return seen - 0x8000 > arrived ? state->rounds + 1 : state->rounds;
}

// Kept only once the packet has proved genuine, and only when it is newer than anything seen, which
// is the whole point: a packet that fails its check, or one that arrives late, must leave the count
// alone. Moving it first let one bad packet put the count permanently out of step, after which
// nothing authenticated again for the rest of the session.
static void keepRounds(SenderState *state, uint16_t number, uint32_t rounds)
{
   if (!state->started) { state->started = 1; state->rounds = rounds; state->lastNumber = number; return; }

   if (rounds == state->rounds + 1) { state->rounds = rounds; state->lastNumber = number; }
   else if (rounds == state->rounds && number > state->lastNumber) state->lastNumber = number;
}
// The count of how many times the packet number has run out cannot be checked by watching a real
// stream: it takes 65536 packets to come round, which is minutes of play, and every session run
// while this was being written was shorter than that. So the sequence that used to break it is
// played through here instead, at startup, where it costs nothing and cannot be skipped.
static void runRolloverSelfTest(void)
{
   // a packet arriving two places late, across the point where the number runs out
   static const uint16_t ARRIVING[] = { 0xFFFD, 0xFFFE, 0xFFFF, 0x0000, 0xFFFE, 0x0001, 0x0002 };
   static const uint32_t WANTED[]   = {      0,      0,      0,      1,      0,      1,      1 };

   SenderState state;
   memSet(&state, 0, sizeof state);

   int failures = 0;
   for (int at = 0; at < (int)(sizeof ARRIVING / sizeof ARRIVING[0]); at++) {
      uint32_t rounds = guessRounds(&state, ARRIVING[at]);
      if (rounds != WANTED[at]) {
         logError(TAG "srtp rollover self test: number 0x%04X read as round %d, should be %d\n",
                  ARRIVING[at], (int)rounds, (int)WANTED[at]);
         failures++;
      }
      keepRounds(&state, ARRIVING[at], rounds);
   }

   // the late one must have left the count alone rather than carrying it forward a second time
   if (state.rounds != 1) {
      logError(TAG "srtp rollover self test: count ended at %d, should be 1\n", (int)state.rounds);
      failures++;
   }

   if (!failures) logInfo(TAG "srtp rollover self test: 0 failures\n");
}

int getRtpPayloadOffset(const void *datagram, int length)
{
   const uint8_t *packet = (const uint8_t *)datagram;
   if (length < HEADER_SIZE) return -1;

   int credited = packet[0] & 0x0F;
   int at = HEADER_SIZE + credited * 4;
   if (at + 4 > length) return -1;

   if (packet[0] & 0x10) {   // extra fields are present, and say their own length in four byte units
      at += 4 + read16(packet + at + 2) * 4;
      if (at > length) return -1;
   }
   return at;
}

int readSrtpPacket(uint8_t *packet, int length)
{
   if (length < HEADER_SIZE + TAG_SIZE) return -1;

   int headerLength = getRtpPayloadOffset(packet, length - TAG_SIZE);
   if (headerLength < 0) return -1;

   uint16_t number = read16(packet + 2);
   uint32_t sender = read32(packet + 8);

   SenderState *state = findSender(sender);
   uint32_t rounds = state ? guessRounds(state, number) : 0;

   int protectedLength = length - TAG_SIZE;

   // section: check it was not altered, before anything is done with it

   uint8_t roundsOnWire[4];
   roundsOnWire[0] = (uint8_t)(rounds >> 24);
   roundsOnWire[1] = (uint8_t)(rounds >> 16);
   roundsOnWire[2] = (uint8_t)(rounds >> 8);
   roundsOnWire[3] = (uint8_t)rounds;

   uint8_t expected[TAG_SIZE];
   signPacket(&media, packet, protectedLength, roundsOnWire, sizeof roundsOnWire, expected);

   uint8_t difference = 0;
   for (int byte = 0; byte < TAG_SIZE; byte++)
      difference |= (uint8_t)(expected[byte] ^ packet[protectedLength + byte]);
   if (difference != 0) return -1;

   // it is genuine, so now the count may move
   if (state) keepRounds(state, number, rounds);

   // section: uncover the contents

   uint64_t fullNumber = ((uint64_t)rounds << 16) | number;
   runCipher(&media, sender, fullNumber, packet + headerLength,
             protectedLength - headerLength);

   return protectedLength;
}

// section: sending reports back
//
// A report is wrapped like a media packet, except that its number is written into it rather than
// counted at both ends, and the top bit beside that number says the contents are hidden.

int openSrtcpSend(const uint8_t *key, const uint8_t *salt)
{
   if (openKeys(&outgoingReports, key, salt, LABEL_REPORT_CIPHER, LABEL_REPORT_AUTH, LABEL_REPORT_SALT) != 0) return -1;

   reportsSent = 0;
   logInfo(TAG "srtp: ready to send reports back\n");
   return 0;
}

int openSrtcpRead(const uint8_t *key, const uint8_t *salt)
{
   return openKeys(&incomingReports, key, salt, LABEL_REPORT_CIPHER, LABEL_REPORT_AUTH, LABEL_REPORT_SALT);
}

int readSrtcpPacket(uint8_t *packet, int length)
{
   int contentLength = length - 4 - TAG_SIZE;   // the number on the end, and the proof after it
   if (contentLength < REPORT_HEADER_SIZE) return -1;

   uint8_t expected[TAG_SIZE];
   signPacket(&incomingReports, packet, contentLength + 4, 0, 0, expected);

   uint8_t difference = 0;
   for (int byte = 0; byte < TAG_SIZE; byte++)
      difference |= (uint8_t)(expected[byte] ^ packet[contentLength + 4 + byte]);
   if (difference != 0) return -1;

   uint32_t number = read32(packet + contentLength);
   if (!(number & 0x80000000)) return contentLength;   // the top bit clear means it was sent plainly

   runCipher(&incomingReports, read32(packet + 4), number & 0x7FFFFFFF,
             packet + REPORT_HEADER_SIZE, contentLength - REPORT_HEADER_SIZE);
   return contentLength;
}

int writeSrtcpPacket(uint8_t *packet, int length, int capacity)
{
   if (length < REPORT_HEADER_SIZE || length + SRTCP_OVERHEAD > capacity) return -1;

   uint32_t sender = read32(packet + 4);
   uint32_t number = ++reportsSent & 0x7FFFFFFF;

   runCipher(&outgoingReports, sender, number, packet + REPORT_HEADER_SIZE,
             length - REPORT_HEADER_SIZE);

   // the number goes on the wire with the top bit set, saying the contents above are hidden
   packet[length + 0] = (uint8_t)((number >> 24) | 0x80);
   packet[length + 1] = (uint8_t)(number >> 16);
   packet[length + 2] = (uint8_t)(number >> 8);
   packet[length + 3] = (uint8_t)number;

   signPacket(&outgoingReports, packet, length + 4, 0, 0, packet + length + 4);
   return length + SRTCP_OVERHEAD;
}
