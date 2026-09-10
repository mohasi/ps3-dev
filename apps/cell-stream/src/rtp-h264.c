// rtp-h264 - see rtp-h264.h.
//
// Each packet carries either a whole piece of the picture, several small ones together, or part of
// a large one. The three are told apart by the first byte of the payload.
//
// The decoder wants each piece introduced by a fixed four byte marker, which is not how they
// travel, so the marker is put back as each piece is written out.

#include "rtp-h264.h"

#include "frame-arena.h"
#include "srtp.h"   // getRtpPayloadOffset
#include "string-utilities.h"

// how the pieces are packed, from RFC 6184. below 24 the packet is one whole piece.
#define PACKING_SEVERAL 24
#define PACKING_PART    28

// the flag on a part saying it is the first of a piece
#define PART_STARTS 0x80

static int which;
static uint8_t *frame;
static int frameLength;
static uint32_t pictureTime;   // every packet of one picture carries the same time
static int building;        // whether anything has been written into the picture being built
static int lost;            // whether a piece went missing, so this picture cannot be used
static uint16_t lastNumber;
static int haveLastNumber;
static int dropped;

static const uint8_t MARKER[4] = { 0, 0, 0, 1 };

void openRtpH264(void)
{
   claimFrameArena("xbox");
   which = 0;
   frame = getFrameArenaSlot(0);
   frameLength = 0;
   building = 0;
   lost = 0;
   haveLastNumber = 0;
   dropped = 0;
}

void closeRtpH264(void)
{
   releaseFrameArena();
   frame = 0;
}

int getRtpH264Dropped(void) { return dropped; }

static uint16_t read16(const uint8_t *data) { return (uint16_t)((data[0] << 8) | data[1]); }

// writes one whole piece into the picture, introduced by the marker the decoder expects
static void appendPiece(const uint8_t *piece, int length)
{
   if (frameLength + (int)sizeof MARKER + length > FRAME_ARENA_SLOT_BYTES) { lost = 1; return; }

   memCopy(frame + frameLength, MARKER, sizeof MARKER);
   frameLength += (int)sizeof MARKER;
   memCopy(frame + frameLength, piece, length);
   frameLength += length;
   building = 1;
}

// several small pieces packed into one packet, each with its length in front of it
static void appendSeveral(const uint8_t *payload, int length)
{
   int at = 1;
   while (at + 2 <= length) {
      int pieceLength = read16(payload + at);
      at += 2;
      if (pieceLength <= 0 || at + pieceLength > length) { lost = 1; return; }
      appendPiece(payload + at, pieceLength);
      at += pieceLength;
   }
}

// one piece too large for a datagram, split across several. the two bytes in front say which piece
// it is and whether this is its start or its end, and the real first byte has to be rebuilt from
// halves of both.
static void appendPart(const uint8_t *payload, int length)
{
   if (length < 3) { lost = 1; return; }

   // the first part of a piece brings the marker and the rebuilt first byte with it; the parts
   // after it are the piece continuing, so they are written on with nothing in between
   if (payload[1] & PART_STARTS) {
      if (frameLength + (int)sizeof MARKER + 1 > FRAME_ARENA_SLOT_BYTES) { lost = 1; return; }
      memCopy(frame + frameLength, MARKER, sizeof MARKER);
      frameLength += (int)sizeof MARKER;
      frame[frameLength++] = (uint8_t)((payload[0] & 0xE0) | (payload[1] & 0x1F));
   }

   if (frameLength + length - 2 > FRAME_ARENA_SLOT_BYTES) { lost = 1; return; }
   memCopy(frame + frameLength, payload + 2, length - 2);
   frameLength += length - 2;
   building = 1;
}

// hands the finished picture to the caller and starts the next one in the other buffer
static int finishPicture(const uint8_t **frameOut)
{
   int finishedLength = frameLength;
   const uint8_t *finished = frame;
   int wasLost = lost;

   which = (which + 1) % FRAME_ARENA_SLOTS;
   frame = getFrameArenaSlot(which);
   frameLength = 0;
   building = 0;
   lost = 0;

   if (wasLost || finishedLength == 0) { dropped += wasLost; return wasLost ? -1 : 0; }
   *frameOut = finished;
   return finishedLength;
}

int feedRtpH264(const uint8_t *packet, int length, const uint8_t **frameOut)
{
   int payloadAt = getRtpPayloadOffset(packet, length);
   if (payloadAt < 0 || payloadAt >= length) return 0;

   // section: notice a missing packet, because a picture with a hole in it cannot be decoded

   uint16_t number = read16(packet + 2);
   if (haveLastNumber && number != (uint16_t)(lastNumber + 1)) lost = 1;
   lastNumber = number;
   haveLastNumber = 1;

   // section: finish the picture being built if this packet belongs to the next one
   //
   // the last packet of a picture is normally marked, but that packet can go missing, so the time
   // stamp is what actually separates one picture from the next: every packet of a picture carries
   // the same one.

   uint32_t time = (uint32_t)((packet[4] << 24) | (packet[5] << 16) | (packet[6] << 8) | packet[7]);
   int completed = 0;
   if (building && time != pictureTime) completed = finishPicture(frameOut);
   pictureTime = time;

   // section: add what this packet carries

   const uint8_t *payload = packet + payloadAt;
   int payloadLength = length - payloadAt;

   int packing = payload[0] & 0x1F;
   if (packing == PACKING_PART) appendPart(payload, payloadLength);
   else if (packing == PACKING_SEVERAL) appendSeveral(payload, payloadLength);
   else appendPiece(payload, payloadLength);

   // a picture already finished above takes precedence: this packet has started the next one
   if (completed != 0) return completed;

   int lastOfPicture = (packet[1] & 0x80) != 0;
   return lastOfPicture ? finishPicture(frameOut) : 0;
}
