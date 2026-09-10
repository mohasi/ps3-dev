// sctp - see sctp.h.
//
// A packet is a short header and then one or more chunks, each saying its own kind and length. The
// header carries a checksum over the whole packet, computed with the checksum written as zero.
//
// The machine opens the association, so this only ever answers: it never sends the opening message
// itself. That keeps the state to three cases rather than the full set.

#include "sctp.h"

#include "dbg.h"
#include "string-utilities.h"

#include <sys/random_number.h>

#define TAG "[cst] "

#define SCTP_PORT 5000   // the port a web browser uses for this, and what the machine addressed

// chunk kinds, from the SCTP registry
#define CHUNK_DATA          0
#define CHUNK_INIT          1
#define CHUNK_INIT_ACK      2
#define CHUNK_SACK          3
#define CHUNK_HEARTBEAT     4
#define CHUNK_HEARTBEAT_ACK 5
#define CHUNK_ABORT         6
#define CHUNK_SHUTDOWN      7
#define CHUNK_COOKIE_ECHO  10
#define CHUNK_COOKIE_ACK   11

#define PARAMETER_STATE_COOKIE 7

// the flags on a data chunk that say a message is whole rather than one piece of a longer one
#define DATA_ENDING    0x01
#define DATA_BEGINNING 0x02
// Says this message may be handed over as soon as it lands. Without it the far end holds every
// later message on that stream until a lost one arrives, and nothing here sends anything twice, so
// one lost controller packet would silence the controller for the rest of the session.
#define DATA_UNORDERED 0x04

#define HEADER_SIZE  12
#define CHUNK_HEADER 4
#define PACKET_MAX   1200   // what fits in one datagram once the encryption around it is counted
#define COOKIE_SIZE  8      // ours only has to come back unchanged, so it is a value we chose
#define STREAM_COUNT 16     // more channels than a game stream opens

static SctpSendFunc sendPacket;

static uint32_t theirTag;      // goes in every packet we send, so they know it is this association
static uint32_t ourTag;        // comes back in every packet they send
static uint32_t nextTsn;       // numbers the messages we send
static uint32_t highestSeen;   // the last of theirs we have acknowledged
static uint16_t nextSequence[STREAM_COUNT];
static uint8_t channelOpen[STREAM_COUNT];   // whether the far end has answered for each channel
static uint8_t channelUnordered[STREAM_COUNT];   // set when the channel was named as taking messages in any order
static uint8_t cookie[COOKIE_SIZE];
static int associationOpen;

// Where arriving messages are kept while the caller reads them. One packet can carry several, and
// the caller reads them all after the call returns, so each needs its own room: sharing one buffer
// handed every message the last one's bytes.
static uint8_t incomingMessages[SCTP_MESSAGES_MAX][PACKET_MAX];

// section: numbers on the wire, always most significant byte first

static uint16_t read16(const uint8_t *data) { return (uint16_t)((data[0] << 8) | data[1]); }

static uint32_t read32(const uint8_t *data)
{
   return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static int write16(uint8_t *out, int at, uint16_t value)
{
   out[at] = (uint8_t)(value >> 8);
   out[at + 1] = (uint8_t)value;
   return at + 2;
}

static int write32(uint8_t *out, int at, uint32_t value)
{
   out[at] = (uint8_t)(value >> 24);
   out[at + 1] = (uint8_t)(value >> 16);
   out[at + 2] = (uint8_t)(value >> 8);
   out[at + 3] = (uint8_t)value;
   return at + 4;
}

// section: the checksum

// This is not the same checksum as everywhere else: SCTP uses its own polynomial, and getting the
// two mixed up produces packets that look right and are refused. Done a bit at a time rather than
// from a table, because the packets here are small and infrequent.
static uint32_t checksumOf(const uint8_t *data, int length)
{
   uint32_t remainder = 0xFFFFFFFF;
   for (int byte = 0; byte < length; byte++) {
      remainder ^= data[byte];
      for (int bit = 0; bit < 8; bit++)
         remainder = (remainder >> 1) ^ (0x82F63B78 & (uint32_t)-(int32_t)(remainder & 1));
   }
   return ~remainder;
}

// The checksum covers the whole packet with its own field written as zero, so checking one means
// summing it the way it was sent. The packet is read-only here, so the four bytes of the field are
// kept aside and zero is fed to the sum in their place.
static int hasGoodChecksum(const uint8_t *packet, int length)
{
   uint32_t carried = (uint32_t)packet[8] | ((uint32_t)packet[9] << 8)
                    | ((uint32_t)packet[10] << 16) | ((uint32_t)packet[11] << 24);

   uint32_t remainder = 0xFFFFFFFF;
   for (int byte = 0; byte < length; byte++) {
      remainder ^= byte >= 8 && byte < 12 ? 0 : packet[byte];
      for (int bit = 0; bit < 8; bit++)
         remainder = (remainder >> 1) ^ (0x82F63B78 & (uint32_t)-(int32_t)(remainder & 1));
   }
   return ~remainder == carried;
}

// the checksum covers the whole packet with its own field written as zero, so it is filled in last
static void signPacket(uint8_t *packet, int length)
{
   write32(packet, 8, 0);
   uint32_t sum = checksumOf(packet, length);

   // this one field is written least significant byte first, unlike everything else in the packet
   packet[8] = (uint8_t)sum;
   packet[9] = (uint8_t)(sum >> 8);
   packet[10] = (uint8_t)(sum >> 16);
   packet[11] = (uint8_t)(sum >> 24);
}

static int writeHeader(uint8_t *out)
{
   int at = write16(out, 0, SCTP_PORT);
   at = write16(out, at, SCTP_PORT);
   at = write32(out, at, theirTag);
   return write32(out, at, 0);   // the checksum, filled in once the rest is written
}

// every chunk is padded out to a multiple of four, and the padding is not counted in its length
static int padTo4(int length) { return (length + 3) & ~3; }

static int isSameBytes(const uint8_t *left, const uint8_t *right, int length)
{
   for (int byte = 0; byte < length; byte++)
      if (left[byte] != right[byte]) return 0;
   return 1;
}

// section: answering

static int sendChunk(int type, int flags, const uint8_t *body, int bodyLength)
{
   uint8_t packet[PACKET_MAX];
   if (HEADER_SIZE + CHUNK_HEADER + bodyLength > PACKET_MAX) return -1;

   int at = writeHeader(packet);
   packet[at++] = (uint8_t)type;
   packet[at++] = (uint8_t)flags;
   at = write16(packet, at, (uint16_t)(CHUNK_HEADER + bodyLength));
   if (bodyLength > 0) {
      memCopy(packet + at, body, bodyLength);
      at += bodyLength;
   }

   while (at < padTo4(at)) packet[at++] = 0;

   signPacket(packet, at);
   return sendPacket(packet, at) < 0 ? -1 : 0;
}

// answers the machine's opening message with our own half of the same information, and a value it
// must hand back before we agree to anything
static int answerInit(const uint8_t *body, int length)
{
   if (length < 16) return -1;

   theirTag = read32(body);
   if (sys_get_random_number(&ourTag, sizeof ourTag) != 0) return -1;
   if (sys_get_random_number(cookie, sizeof cookie) != 0) return -1;
   if (sys_get_random_number(&nextTsn, sizeof nextTsn) != 0) return -1;
   highestSeen = read32(body + 12) - 1;   // their first message will be the one after this

   uint8_t reply[64];
   int at = write32(reply, 0, ourTag);
   at = write32(reply, at, 0x00020000);   // how much we are willing to hold unread, 128 KB
   at = write16(reply, at, STREAM_COUNT);
   at = write16(reply, at, STREAM_COUNT);
   at = write32(reply, at, nextTsn);

   at = write16(reply, at, PARAMETER_STATE_COOKIE);
   at = write16(reply, at, (uint16_t)(4 + COOKIE_SIZE));
   memCopy(reply + at, cookie, COOKIE_SIZE);
   at += COOKIE_SIZE;

   logInfo(TAG "sctp: the machine opened a channel, answering\n");
   return sendChunk(CHUNK_INIT_ACK, 0, reply, at);
}

// says which of their messages we have received, so they stop sending it again
static int sendAcknowledgement(void)
{
   uint8_t body[12];
   int at = write32(body, 0, highestSeen);
   at = write32(body, at, 0x00020000);
   at = write16(body, at, 0);   // nothing missing in the middle
   at = write16(body, at, 0);   // and nothing arrived twice
   return sendChunk(CHUNK_SACK, 0, body, at);
}

// section: naming the channels

// the two messages of the naming protocol, from RFC 8832
#define CHANNEL_ACK  0x02
#define CHANNEL_OPEN 0x03

// How a channel behaves, from the same place. The top bit is what says messages may be handed over
// in whatever order they land, and a channel named that way must have every message marked the same
// way when it is sent, or the far end holds each one back waiting for the one before it.
#define CHANNEL_RELIABLE_ORDERED     0x00
#define CHANNEL_LOSSY_UNORDERED      0x81

int openSctpChannel(int channel, const char *label, const char *protocol, int ordered)
{
   if (channel < 0 || channel >= STREAM_COUNT) return -1;

   int labelLength = getStrLen(label);
   int protocolLength = getStrLen(protocol);

   uint8_t body[128];
   if (12 + labelLength + protocolLength > (int)sizeof body) return -1;

   channelUnordered[channel] = !ordered;

   int at = 0;
   body[at++] = CHANNEL_OPEN;
   body[at++] = (uint8_t)(ordered ? CHANNEL_RELIABLE_ORDERED : CHANNEL_LOSSY_UNORDERED);
   at = write16(body, at, 0);   // no channel matters more than another here
   at = write32(body, at, 0);   // and where messages are given up on, they get no second attempt
   at = write16(body, at, (uint16_t)labelLength);
   at = write16(body, at, (uint16_t)protocolLength);
   memCopy(body + at, label, labelLength);
   at += labelLength;
   memCopy(body + at, protocol, protocolLength);
   at += protocolLength;

   return sendSctpMessage(channel, SCTP_PAYLOAD_CONTROL, body, at);
}

int isSctpChannelOpen(int channel)
{
   return channel >= 0 && channel < STREAM_COUNT && channelOpen[channel];
}

// section: reading

// one message the machine sent. pieces of a longer message are not put back together: a game
// stream's messages are short, and one that arrived split would be a surprise worth seeing.
static int readData(const uint8_t *body, int length, SctpMessage *message, int slot)
{
   if (length < 12) return -1;

   highestSeen = read32(body);

   // the number is sixteen bits on the wire and only a handful of channels exist, so anything
   // outside the range this end offered is refused rather than used to reach past the arrays it
   // would otherwise index
   message->channel = read16(body + 4);
   if (message->channel < 0 || message->channel >= STREAM_COUNT) return -1;

   message->kind = (SctpPayloadKind)read32(body + 8);
   message->length = length - 12;
   if (message->length > PACKET_MAX) return -1;

   memCopy(incomingMessages[slot], body + 12, message->length);
   message->data = incomingMessages[slot];
   return 0;
}

// either the far end answering a channel we named, or naming one of its own, which is answered
// the same way a web browser would
static void takeChannelNaming(int channel, const uint8_t *data, int length)
{
   if (data[0] == CHANNEL_ACK) {
      channelOpen[channel] = 1;
      logInfo(TAG "sctp: channel %d ready\n", channel);
      return;
   }
   if (data[0] != CHANNEL_OPEN || length < 12) return;

   // the byte after says how the far end wants this channel treated, and its top bit is the one
   // that says messages may be handed over in whatever order they land
   channelUnordered[channel] = (data[1] & 0x80) != 0;

   uint8_t answer = CHANNEL_ACK;
   if (sendSctpMessage(channel, SCTP_PAYLOAD_CONTROL, &answer, 1) == 0) {
      channelOpen[channel] = 1;
      logInfo(TAG "sctp: the machine named channel %d\n", channel);
   }
}

int feedSctp(const void *packet, int length, SctpMessage *messages, int capacity)
{
   const uint8_t *data = (const uint8_t *)packet;
   if (length < HEADER_SIZE) return -1;
   if (capacity > SCTP_MESSAGES_MAX) capacity = SCTP_MESSAGES_MAX;   // one room per message, no more

   if (!hasGoodChecksum(data, length)) return -1;

   // Every packet after the opening exchange carries the number this end chose, which is what
   // separates this conversation from anything else arriving. The opening message is the exception:
   // it is sent before either end has a number, so it carries zero.
   int opening = data[HEADER_SIZE] == CHUNK_INIT;
   if (ourTag && !opening && read32(data + 4) != ourTag) return -1;

   int found = 0;
   int owesAcknowledgement = 0;
   int at = HEADER_SIZE;

   while (at + CHUNK_HEADER <= length) {
      int type = data[at];
      int flags = data[at + 1];
      int chunkLength = read16(data + at + 2);
      if (chunkLength < CHUNK_HEADER || at + chunkLength > length) break;

      const uint8_t *body = data + at + CHUNK_HEADER;
      int bodyLength = chunkLength - CHUNK_HEADER;
      at += padTo4(chunkLength);

      switch (type) {
      case CHUNK_INIT:
         if (answerInit(body, bodyLength) != 0) return -1;
         break;

      // the value handed out with the opening answer has to come back unchanged, which is the
      // point of sending one: agreeing to talk to whoever asks would make the check pointless
      case CHUNK_COOKIE_ECHO:
         if (bodyLength != COOKIE_SIZE || !isSameBytes(body, cookie, COOKIE_SIZE)) {
            logWarn(TAG "sctp: the opening value came back wrong, so the packet is not ours\n");
            return -1;
         }
         associationOpen = 1;
         logInfo(TAG "sctp: channel open\n");
         if (sendChunk(CHUNK_COOKIE_ACK, 0, 0, 0) != 0) return -1;
         break;

      case CHUNK_DATA:
         owesAcknowledgement = 1;
         if (found >= capacity || readData(body, bodyLength, &messages[found], found) != 0) break;

         if ((flags & (DATA_BEGINNING | DATA_ENDING)) != (DATA_BEGINNING | DATA_ENDING))
            logWarn(TAG "sctp: a message arrived in pieces, which nothing here puts back together\n");

         // a channel being named is this layer's own business, not the caller's
         if (messages[found].kind == SCTP_PAYLOAD_CONTROL && messages[found].length > 0) {
            takeChannelNaming(messages[found].channel, messages[found].data, messages[found].length);
            break;
         }
         found++;
         break;

      case CHUNK_HEARTBEAT:
         if (sendChunk(CHUNK_HEARTBEAT_ACK, 0, body, bodyLength) != 0) return -1;
         break;

      case CHUNK_ABORT:
      case CHUNK_SHUTDOWN:
         logWarn(TAG "sctp: the machine closed the channel\n");
         associationOpen = 0;
         return -1;
      }
   }

   if (owesAcknowledgement) sendAcknowledgement();
   return found;
}

// section: the API

int openSctp(SctpSendFunc send)
{
   if (!send) return -1;
   sendPacket = send;

   theirTag = 0;
   ourTag = 0;
   associationOpen = 0;
   memSet(nextSequence, 0, sizeof nextSequence);
   memSet(channelOpen, 0, sizeof channelOpen);
   memSet(channelUnordered, 0, sizeof channelUnordered);
   return 0;
}

int isSctpOpen(void) { return associationOpen; }

int sendSctpMessage(int channel, SctpPayloadKind kind, const void *data, int length)
{
   if (!associationOpen || channel < 0 || channel >= STREAM_COUNT) return -1;

   uint8_t body[PACKET_MAX];
   if (12 + length > (int)sizeof body) return -1;

   // naming a channel is part of setting it up and has to arrive in order; what flows afterwards
   // follows what the channel was named as
   int unordered = kind != SCTP_PAYLOAD_CONTROL && channelUnordered[channel];

   int at = write32(body, 0, nextTsn++);
   at = write16(body, at, (uint16_t)channel);
   at = write16(body, at, unordered ? 0 : nextSequence[channel]++);   // the number means nothing when unordered
   at = write32(body, at, (uint32_t)kind);
   memCopy(body + at, data, length);
   at += length;

   return sendChunk(CHUNK_DATA, DATA_BEGINNING | DATA_ENDING | (unordered ? DATA_UNORDERED : 0), body, at);
}

void runSctpSelfTest(void)
{
   // the published check value for this checksum: the nine characters "123456789"
   static const uint8_t INPUT[] = "123456789";
   uint32_t got = checksumOf(INPUT, 9);

   if (got != 0xE3069283)
      logError(TAG "sctp self test: checksum is 0x%08X, should be 0xE3069283\n", got);
   else
      logInfo(TAG "sctp self test: 0 failures\n");
}
