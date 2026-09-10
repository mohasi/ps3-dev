// rtcp - see rtcp.h.
//
// The shape of the report is RFC 3550: one block per source saying how much of what it sent was
// received, how far apart the packets arrived compared with how far apart they were sent, and how
// far the numbering has got. A name for this end follows, which the standard requires alongside.

#include "rtcp.h"

#include "srtp.h"   // getRtpPayloadOffset, writeSrtcpPacket
#include "string-utilities.h"

#define SOURCE_MAX 2   // the picture and the sound

#define REPORT_EVERY_US 1000000

#define KIND_SENDER   200
#define KIND_RECEIVER 201
#define KIND_NAME     202

#define VIDEO_KIND 102   // what the machine's description called each, and what its clock runs at
#define VIDEO_CLOCK 90000
#define SOUND_CLOCK 48000

// This end has to call itself something. Nothing else in the exchange refers to it, so a fixed
// value is enough; RFC 3550 only asks that it not collide with a source in the same session, and
// the machine's own are chosen from a far larger range.
#define OUR_SENDER 0x50533300   // "PS3"

static const char CNAME[] = "cellstream";

typedef struct {
   uint32_t sender;
   uint32_t clock;

   uint32_t baseNumber;      // the first number seen, so how many were expected can be worked out
   uint32_t highestNumber;   // the highest seen, counting the rounds the sixteen bit number has run out

   uint32_t received;
   uint32_t receivedAtLastReport;
   uint32_t expectedAtLastReport;

   uint32_t jitter;          // how uneven the arrivals are, in the sender's own clock units
   uint32_t lastSentAt;      // the last packet's time as the sender stamped it
   uint64_t lastArrivedAtUs;

   uint32_t machineClock;         // the middle of the machine's clock reading in its last report
   uint64_t machineClockAtUs;     // when that report reached here, so the wait can be taken off

   int used;
} Source;

static Source sources[SOURCE_MAX];
static uint64_t lastReportAtUs;

void openRtcp(void)
{
   memSet(sources, 0, sizeof sources);
   lastReportAtUs = 0;
}

static uint16_t read16(const uint8_t *data) { return (uint16_t)((data[0] << 8) | data[1]); }

static uint32_t read32(const uint8_t *data)
{
   return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static void write32(uint8_t *out, uint32_t value)
{
   out[0] = (uint8_t)(value >> 24);
   out[1] = (uint8_t)(value >> 16);
   out[2] = (uint8_t)(value >> 8);
   out[3] = (uint8_t)value;
}

static Source *getSource(uint32_t sender, uint32_t clock)
{
   for (int index = 0; index < SOURCE_MAX; index++)
      if (sources[index].used && sources[index].sender == sender) return &sources[index];

   for (int index = 0; index < SOURCE_MAX; index++) {
      if (sources[index].used) continue;
      sources[index].used = 1;
      sources[index].sender = sender;
      sources[index].clock = clock;
      return &sources[index];
   }
   return 0;
}

// how far apart two packets arrived compared with how far apart they were sent, smoothed. a
// sender reads this as how steady the path is.
static void countJitter(Source *source, uint32_t sentAt, uint64_t arrivedAtUs)
{
   if (source->lastArrivedAtUs) {
      int64_t arrivedApart = (int64_t)((arrivedAtUs - source->lastArrivedAtUs) * source->clock / 1000000);
      int64_t sentApart = (int32_t)(sentAt - source->lastSentAt);
      int64_t difference = arrivedApart - sentApart;
      if (difference < 0) difference = -difference;

      source->jitter += (uint32_t)((difference - (int64_t)source->jitter) / 16);
   }

   source->lastSentAt = sentAt;
   source->lastArrivedAtUs = arrivedAtUs;
}

void noteRtcpPacket(const void *packet, int length, uint64_t arrivedAtUs)
{
   const uint8_t *rtp = (const uint8_t *)packet;
   if (getRtpPayloadOffset(rtp, length) < 0) return;

   uint32_t clock = (rtp[1] & 0x7F) == VIDEO_KIND ? VIDEO_CLOCK : SOUND_CLOCK;
   Source *source = getSource(read32(rtp + 8), clock);
   if (!source) return;

   uint16_t number = read16(rtp + 2);

   if (source->received == 0) {
      source->baseNumber = number;
      source->highestNumber = number;
   } else {
      // Only sixteen bits of the number are on the wire, so which round a packet belongs to is a
      // guess, made the way RFC 3711 Appendix A makes it. Deciding it from the jump alone counted a
      // round twice whenever a packet arrived late across the point where the number runs out, and
      // the report then claimed 65536 packets lost, which is the one thing that would make the
      // machine drop the bitrate.
      int seen = source->highestNumber & 0xFFFF, arrived = number;
      uint32_t rounds = source->highestNumber >> 16;
      if (seen < 0x8000) { if (arrived - seen > 0x8000 && rounds > 0) rounds--; }
      else if (seen - 0x8000 > arrived) rounds++;

      uint32_t extended = (rounds << 16) | number;
      if (extended > source->highestNumber) source->highestNumber = extended;
   }

   source->received++;
   countJitter(source, read32(rtp + 4), arrivedAtUs);
}

// The machine's own report says what its clock read when it sent it. Sending that back, with the
// time it sat here taken off, is how the machine works out the round trip. Only the middle thirty
// two bits of the reading are used, which is what the report format carries.
void noteRtcpSenderReport(const void *packet, int length, uint64_t arrivedAtUs)
{
   const uint8_t *report = (const uint8_t *)packet;

   // several reports travel in one packet, one after another, each saying its own length
   int at = 0;
   while (at + 4 <= length) {
      int reportLength = (read16(report + at + 2) + 1) * 4;
      if (reportLength <= 0 || at + reportLength > length) return;

      if (report[at + 1] == KIND_SENDER && reportLength >= 20) {
         for (int index = 0; index < SOURCE_MAX; index++) {
            if (!sources[index].used || sources[index].sender != read32(report + at + 4)) continue;
            sources[index].machineClock = read32(report + at + 10);   // the middle of the eight byte reading
            sources[index].machineClockAtUs = arrivedAtUs;
         }
      }
      at += reportLength;
   }
}

// one block per source, in the shape RFC 3550 gives
static void writeReportBlock(uint8_t *out, Source *source, uint64_t nowUs)
{
   uint32_t highest = source->highestNumber;
   uint32_t expected = highest - source->baseNumber + 1;

   int32_t missingNow = (int32_t)(expected - source->expectedAtLastReport)
                      - (int32_t)(source->received - source->receivedAtLastReport);
   uint32_t expectedNow = expected - source->expectedAtLastReport;

   int32_t missingEver = (int32_t)(expected - source->received);
   if (missingEver < 0) missingEver = 0;

   // The share lost since the last report, as a fraction of 256, and capped: losing everything
   // works out as exactly 256, which is the one reading that must not be wrong and the one that
   // truncates to zero in a byte.
   uint32_t share = 0;
   if (missingNow > 0 && expectedNow > 0) share = (uint32_t)missingNow * 256 / expectedNow;
   if (share > 255) share = 255;

   write32(out, source->sender);
   out[4] = (uint8_t)share;
   out[5] = (uint8_t)(missingEver >> 16);
   out[6] = (uint8_t)(missingEver >> 8);
   out[7] = (uint8_t)missingEver;
   write32(out + 8, highest);
   write32(out + 12, source->jitter);
   // the machine's own clock reading, and how long it has sat here, in units of 1/65536 of a second
   uint32_t waited = source->machineClockAtUs ? (uint32_t)((nowUs - source->machineClockAtUs) * 65536 / 1000000) : 0;
   write32(out + 16, source->machineClock);
   write32(out + 20, source->machineClock ? waited : 0);

   source->expectedAtLastReport = expected;
   source->receivedAtLastReport = source->received;
}

int getRtcpReport(uint64_t nowUs, uint8_t *out, int capacity)
{
   if (lastReportAtUs && nowUs - lastReportAtUs < REPORT_EVERY_US) return 0;

   int counted = 0;
   for (int index = 0; index < SOURCE_MAX; index++) counted += sources[index].used != 0;
   if (!counted) return 0;
   lastReportAtUs = nowUs;

   // section: what arrived

   int nameLength = getStrLen(CNAME);
   int namePadded = (4 + 2 + nameLength + 1 + 3) & ~3;   // sender, the item, its end, to a multiple of four
   int length = 8 + counted * 24 + 4 + namePadded;
   if (length + SRTCP_OVERHEAD > capacity) return 0;

   out[0] = (uint8_t)(0x80 | counted);
   out[1] = KIND_RECEIVER;
   out[2] = 0;
   out[3] = (uint8_t)(1 + counted * 6);   // its size in four byte units, not counting the first
   write32(out + 4, OUR_SENDER);

   int at = 8;
   for (int index = 0; index < SOURCE_MAX; index++) {
      if (!sources[index].used) continue;
      writeReportBlock(out + at, &sources[index], nowUs);
      at += 24;
   }

   // section: what to call this end, which has to follow

   out[at + 0] = 0x81;
   out[at + 1] = KIND_NAME;
   out[at + 2] = 0;
   out[at + 3] = (uint8_t)(namePadded / 4);
   write32(out + at + 4, OUR_SENDER);

   int nameAt = at + 8;
   out[nameAt] = 1;   // the item is a name for this end
   out[nameAt + 1] = (uint8_t)nameLength;
   memCopy(out + nameAt + 2, CNAME, nameLength);
   memSet(out + nameAt + 2 + nameLength, 0, length - (nameAt + 2 + nameLength));

   return writeSrtcpPacket(out, length, capacity);
}
