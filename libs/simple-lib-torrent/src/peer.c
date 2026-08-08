// peer - the wire protocol between two holders of a torrent (BEP 3), a step at a time.
//
// After a handshake naming the torrent, a peer says whether it will serve us. Only then can blocks
// be asked for. Nothing here blocks, so the caller can keep several peers going at once.

#include "peer.h"

#include "bencode.h"
#include "dbg.h"
#include "string-utilities.h"
#include "torrent-net.h"

#define TAG "[bt] "

#define PROTOCOL_NAME    "BitTorrent protocol"
#define HANDSHAKE_LENGTH 68

#define CONNECT_TIMEOUT_MS  6000   // an unreachable peer is dropped quickly so the next one is tried
#define QUIET_TIMEOUT_MS   60000   // a peer that is choking us says nothing for a while, and is worth keeping
#define KEEPALIVE_MS       30000   // long enough to be quiet that the other end would drop us
#define SEND_TIMEOUT_MS     8000

typedef enum {
   MESSAGE_CHOKE = 0,
   MESSAGE_UNCHOKE = 1,
   MESSAGE_INTERESTED = 2,
   MESSAGE_HAVE = 4,       // one piece it has just finished
   MESSAGE_BITFIELD = 5,   // every piece it holds, sent once after the handshake
   MESSAGE_REQUEST = 6,
   MESSAGE_PIECE = 7,
   MESSAGE_EXTENDED = 20   // BEP 10, which is what carries the description request underneath
} PeerMessage;

#define EXTENSION_FLAG_BYTE 25     // of the handshake's eight spare bytes, the one BEP 10 claims
#define EXTENSION_FLAG      0x10
#define EXTENSION_HANDSHAKE 0      // our numbering: 0 is always the handshake
#define DESCRIPTION_MESSAGE 1      // what we ask peers to number description messages

// what a peer sends back about the description: 1 means the data is here, 2 means it will not serve it
#define DESCRIPTION_DATA 1

static uint32_t readBe32(const uint8_t *bytes)
{
   return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void writeBe32(uint8_t *out, uint32_t value)
{
   out[0] = (uint8_t)(value >> 24);
   out[1] = (uint8_t)(value >> 16);
   out[2] = (uint8_t)(value >> 8);
   out[3] = (uint8_t)value;
}

// as much of what is left to read past as the buffer holds
static int getChunkLength(int length)
{
   return length < PEER_MESSAGE_MAX ? length : PEER_MESSAGE_MAX;
}

static void expect(PeerLink *link, int bytes, int readingLength)
{
   link->have = 0;
   link->need = bytes;
   link->readingLength = readingLength;
}

static void die(PeerLink *link)
{
   closePeer(link);
   link->state = PEER_DEAD;
}

static int sendMessage(PeerLink *link, PeerMessage id, const uint8_t *payload, int payloadLength)
{
   const TorrentNetwork *network = getTorrentNetwork();

   uint8_t header[5];
   writeBe32(header, (uint32_t)(payloadLength + 1));
   header[4] = (uint8_t)id;

   if (network->sendTcp(link->handle, header, sizeof header, SEND_TIMEOUT_MS) != (int)sizeof header) return -1;
   if (payloadLength > 0 && network->sendTcp(link->handle, payload, payloadLength, SEND_TIMEOUT_MS) != payloadLength)
      return -1;

   link->spokeAtMs = network->getNowMs();
   return 0;
}

// four zero bytes and nothing else: it says we are still here while a peer is choking us
static int sendKeepAlive(PeerLink *link)
{
   const TorrentNetwork *network = getTorrentNetwork();
   uint8_t nothing[4] = { 0, 0, 0, 0 };

   if (network->sendTcp(link->handle, nothing, sizeof nothing, SEND_TIMEOUT_MS) != (int)sizeof nothing) return -1;

   link->spokeAtMs = network->getNowMs();
   return 0;
}

// a bencoded string: how long it is, a colon, then the text itself
static int appendBencodeText(uint8_t *out, const char *text)
{
   char digits[12];
   int textLength = getStrLen(text);
   int digitCount = intToDec(textLength, digits);

   memCopy(out, digits, digitCount);
   out[digitCount] = ':';
   memCopy(out + digitCount + 1, text, textLength);
   return digitCount + 1 + textLength;
}

// a bencoded integer inside a dictionary we are building by hand
static int appendBencodeInt(uint8_t *out, int offset, const char *key, int value)
{
   char digits[16];
   int length = intToDec(value, digits);

   offset += appendBencodeText(out + offset, key);
   out[offset++] = 'i';
   memCopy(out + offset, digits, length);
   offset += length;
   out[offset++] = 'e';
   return offset;
}

// the first thing both ends send: the protocol's name, the torrent, and who we are
static int sendHandshake(PeerLink *link, const uint8_t *infoHash, const uint8_t *peerId)
{
   int nameLength = getStrLen(PROTOCOL_NAME);

   uint8_t handshake[HANDSHAKE_LENGTH];
   memSet(handshake, 0, sizeof handshake);
   handshake[0] = (uint8_t)nameLength;
   memCopy(handshake + 1, PROTOCOL_NAME, nameLength);
   handshake[EXTENSION_FLAG_BYTE] = EXTENSION_FLAG;   // say we can talk about descriptions
   memCopy(handshake + 28, infoHash, SHA1_LENGTH);
   memCopy(handshake + 48, peerId, PEER_ID_LENGTH);

   return getTorrentNetwork()->sendTcp(link->handle, handshake, sizeof handshake, SEND_TIMEOUT_MS) == HANDSHAKE_LENGTH
             ? 0 : -1;
}

static int isTheirGreetingGood(PeerLink *link, const uint8_t *infoHash)
{
   link->extended = (link->message[EXTENSION_FLAG_BYTE] & EXTENSION_FLAG) != 0;

   int nameLength = getStrLen(PROTOCOL_NAME);
   if (link->message[0] != (uint8_t)nameLength) return 0;
   if (findBytes((const char *)link->message + 1, nameLength, PROTOCOL_NAME, nameLength) != 0) return 0;

   // a peer answering about a different torrent is no use to us
   for (int index = 0; index < SHA1_LENGTH; index++)
      if (link->message[28 + index] != infoHash[index]) return 0;

   return 1;
}

int openPeer(PeerLink *link, const PeerAddress *address)
{
   const TorrentNetwork *network = getTorrentNetwork();
   if (!network) return -1;

   memSet(link, 0, sizeof *link);
   link->address = *address;
   link->choked = 1;
   link->quietSinceMs = network->getNowMs();
   link->state = PEER_CONNECTING;
   link->handle = network->openTcp(address->address, address->port);

   if (link->handle < 0) {
      link->state = PEER_DEAD;
      return -1;
   }

   return 0;
}

void closePeer(PeerLink *link)
{
   if (link->handle < 0) return;

   getTorrentNetwork()->closeTcp(link->handle);
   link->handle = -1;
}

const uint8_t *getPeerBlock(const PeerLink *link)
{
   return link->message + link->dataOffset;
}

int peerHasPiece(const PeerLink *link, int pieceIndex)
{
   if (!link->piecesHeldKnown || pieceIndex < 0) return 1;
   if (pieceIndex / 8 >= PEER_HAVE_MAX) return 1;

   return (link->piecesHeld[pieceIndex / 8] & (0x80 >> (pieceIndex % 8))) != 0;
}

int requestPeerBlock(PeerLink *link, int pieceIndex, int begin, int length)
{
   uint8_t payload[12];
   writeBe32(payload, (uint32_t)pieceIndex);
   writeBe32(payload + 4, (uint32_t)begin);
   writeBe32(payload + 8, (uint32_t)length);

   if (sendMessage(link, MESSAGE_REQUEST, payload, sizeof payload) != 0) {
      die(link);
      return -1;
   }

   link->requests++;
   return 0;
}

// BEP 10: tell the peer what we can talk about and what to number it
static int sendExtensionHandshake(PeerLink *link)
{
   uint8_t payload[64];
   int offset = 0;

   payload[offset++] = EXTENSION_HANDSHAKE;
   payload[offset++] = 'd';
   offset += appendBencodeText(payload + offset, "m");
   payload[offset++] = 'd';
   offset = appendBencodeInt(payload, offset, "ut_metadata", DESCRIPTION_MESSAGE);
   payload[offset++] = 'e';
   payload[offset++] = 'e';

   return sendMessage(link, MESSAGE_EXTENDED, payload, offset);
}

int requestPeerDescription(PeerLink *link, int descriptionPiece)
{
   if (link->descriptionMessageId <= 0) return -1;

   uint8_t payload[64];
   int offset = 0;

   payload[offset++] = (uint8_t)link->descriptionMessageId;
   payload[offset++] = 'd';
   offset = appendBencodeInt(payload, offset, "msg_type", 0);   // 0 asks for a piece of it
   offset = appendBencodeInt(payload, offset, "piece", descriptionPiece);
   payload[offset++] = 'e';

   if (sendMessage(link, MESSAGE_EXTENDED, payload, offset) != 0) {
      die(link);
      return -1;
   }

   link->requests++;
   return 0;
}

// what a peer answers our extension handshake with: which numbers it wants, and how long its
// description is
static void takeExtensionHandshake(PeerLink *link, int payloadLength)
{
   const uint8_t *document = link->message + 2;
   int length = payloadLength - 1;

   BencodeValue root, numbers, value;
   if (readBencode(document, length, 0, &root) < 0 || root.kind != BENCODE_DICTIONARY) return;

   if (findBencodeMember(document, length, &root, "m", &numbers) == 0 &&
       findBencodeMember(document, length, &numbers, "ut_metadata", &value) == 0)
      link->descriptionMessageId = (int)getBencodeInteger(document, &value);

   if (findBencodeMember(document, length, &root, "metadata_size", &value) == 0)
      link->descriptionSize = (int)getBencodeInteger(document, &value);
}

// a piece of the description: a small dictionary saying which piece, then its bytes
static PeerEvent takeDescriptionPiece(PeerLink *link, int payloadLength)
{
   const uint8_t *document = link->message + 2;
   int length = payloadLength - 1;

   BencodeValue root, value;
   int dictionaryEnd = readBencode(document, length, 0, &root);
   if (dictionaryEnd < 0 || root.kind != BENCODE_DICTIONARY) return PEER_EVENT_NONE;

   if (findBencodeMember(document, length, &root, "msg_type", &value) != 0 ||
       getBencodeInteger(document, &value) != DESCRIPTION_DATA)
      return PEER_EVENT_NONE;

   if (findBencodeMember(document, length, &root, "piece", &value) != 0) return PEER_EVENT_NONE;

   link->blockPiece = (int)getBencodeInteger(document, &value);
   link->dataOffset = 2 + dictionaryEnd;   // where the bytes start inside the message
   link->blockLength = length - dictionaryEnd;
   if (link->requests > 0) link->requests--;

   return link->blockLength > 0 ? PEER_EVENT_DESCRIPTION : PEER_EVENT_NONE;
}

// the connection is up: greet them, say we want something, and start reading
static void startGreeting(PeerLink *link, const TorrentMeta *meta, const uint8_t *peerId)
{
   if (sendHandshake(link, meta->infoHash, peerId) != 0 || sendMessage(link, MESSAGE_INTERESTED, 0, 0) != 0) {
      die(link);
      return;
   }

   link->state = PEER_GREETING;
   expect(link, HANDSHAKE_LENGTH, 0);
}

// what a fully read message means. returns the event to report.
static PeerEvent takeMessage(PeerLink *link)
{
   uint8_t id = link->message[0];

   if (id == MESSAGE_CHOKE) {
      // anything asked for is dropped when a peer chokes us, so the caller has to ask again
      link->choked = 1;
      link->requests = 0;
      return PEER_EVENT_NONE;
   }

   // the list of what it holds, and later the pieces it finishes one at a time
   if (id == MESSAGE_BITFIELD) {
      int length = link->need - 1;
      if (length > 0 && length <= PEER_HAVE_MAX) {
         memCopy(link->piecesHeld, link->message + 1, length);
         memSet(link->piecesHeld + length, 0, PEER_HAVE_MAX - length);
         link->piecesHeldKnown = 1;
      }

      return PEER_EVENT_NONE;
   }

   if (id == MESSAGE_HAVE && link->need >= 5) {
      int piece = (int)readBe32(link->message + 1);
      if (link->piecesHeldKnown && piece >= 0 && piece / 8 < PEER_HAVE_MAX)
         link->piecesHeld[piece / 8] |= 0x80 >> (piece % 8);

      return PEER_EVENT_NONE;
   }

   if (id == MESSAGE_UNCHOKE) {
      char address[16];
      formatIpv4(address, sizeof address, link->address.address);
      logTrace(TAG "peer: %s:%d will serve us\n", address, link->address.port);

      link->choked = 0;
      return PEER_EVENT_NONE;
   }

   // a message the extension protocol carries: its own small id says which
   if (id == MESSAGE_EXTENDED && link->need >= 2) {
      if (link->message[1] == EXTENSION_HANDSHAKE) takeExtensionHandshake(link, link->need - 1);
      else if (link->message[1] == DESCRIPTION_MESSAGE) return takeDescriptionPiece(link, link->need - 1);

      return PEER_EVENT_NONE;
   }

   if (id != MESSAGE_PIECE || link->need < 9) return PEER_EVENT_NONE;

   link->blockPiece = (int)readBe32(link->message + 1);
   link->blockBegin = (int)readBe32(link->message + 5);
   link->blockLength = link->need - 9;
   link->dataOffset = 9;   // past the id, the piece number and where in the piece it goes
   if (link->requests > 0) link->requests--;

   return PEER_EVENT_BLOCK;
}

PeerEvent servicePeer(PeerLink *link, const TorrentMeta *meta, const uint8_t *peerId)
{
   const TorrentNetwork *network = getTorrentNetwork();
   if (link->state == PEER_DEAD) return PEER_EVENT_NONE;

   uint64_t now = network->getNowMs();

   // section: still being set up
   if (network->isTcpFailed(link->handle)) {
      die(link);
      return PEER_EVENT_DIED;
   }

   if (link->state == PEER_CONNECTING) {
      if (network->isTcpConnecting(link->handle)) {
         if (now - link->quietSinceMs < CONNECT_TIMEOUT_MS) return PEER_EVENT_NONE;

         die(link);
         return PEER_EVENT_DIED;
      }

      link->quietSinceMs = now;
      startGreeting(link, meta, peerId);
      return link->state == PEER_DEAD ? PEER_EVENT_DIED : PEER_EVENT_NONE;
   }

   // section: say something now and then, or the other end drops us while we wait to be served
   if (link->state == PEER_READY && now - link->spokeAtMs > KEEPALIVE_MS && sendKeepAlive(link) != 0) {
      die(link);
      return PEER_EVENT_DIED;
   }

   // section: read whatever has arrived
   int taken = network->receiveTcp(link->handle, link->message + link->have, link->need - link->have);
   if (taken < 0) {
      die(link);
      return PEER_EVENT_DIED;
   }

   if (taken == 0) {
      if (now - link->quietSinceMs < QUIET_TIMEOUT_MS) return PEER_EVENT_NONE;

      die(link);
      return PEER_EVENT_DIED;
   }

   link->quietSinceMs = now;
   link->have += taken;
   if (link->have < link->need) return PEER_EVENT_NONE;

   // section: a whole thing is in hand
   if (link->state == PEER_GREETING) {
      if (!isTheirGreetingGood(link, meta->infoHash)) {
         die(link);
         return PEER_EVENT_DIED;
      }

      char address[16];
      formatIpv4(address, sizeof address, link->address.address);
      logTrace(TAG "peer: %s:%d is talking to us\n", address, link->address.port);

      if (link->extended && sendExtensionHandshake(link) != 0) {
         die(link);
         return PEER_EVENT_DIED;
      }

      link->state = PEER_READY;
      link->readyAtMs = now;
      expect(link, 4, 1);
      return PEER_EVENT_NONE;
   }

   // section: reading past something we have no use for, such as a long list of what a peer holds
   if (link->discard > 0) {
      link->discard -= link->have;
      expect(link, link->discard > 0 ? getChunkLength(link->discard) : 4, link->discard <= 0);
      return PEER_EVENT_NONE;
   }

   if (link->readingLength) {
      uint32_t messageLength = readBe32(link->message);

      if (messageLength == 0) { expect(link, 4, 1); return PEER_EVENT_NONE; }   // a keep-alive carries nothing

      if (messageLength > PEER_MESSAGE_MAX) {
         if (messageLength > 0x7FFFFFFF) { die(link); return PEER_EVENT_DIED; }   // not a length we can read past

         link->discard = (int)messageLength;
         expect(link, getChunkLength(link->discard), 0);
         return PEER_EVENT_NONE;
      }

      expect(link, (int)messageLength, 0);
      return PEER_EVENT_NONE;
   }

   PeerEvent event = takeMessage(link);
   expect(link, 4, 1);
   return event;
}
