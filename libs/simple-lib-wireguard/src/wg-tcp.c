#include "wg-tcp.h"

#include "dbg.h"
#include "string-utilities.h"
#include "wg-bytes.h"
#include "wg-ip.h"

#define TAG "[wg] "

#define IP_PROTOCOL_TCP   6
#define TCP_HEADER_LENGTH 20

#define OPTION_END            0
#define OPTION_NOTHING        1
#define OPTION_SEGMENT_MAX    2
#define OPTION_WINDOW_SHIFT   3
#define OPTION_SACK_PERMITTED 4
#define OPTION_SACK           5

#define HELLO_OPTIONS_LENGTH 12
#define SACK_BLOCK_MAX        3   // three runs is what fits beside the options a data segment carries

#define RESEND_LIMIT        8      // attempts at the same data before the connection is given up on
#define HELLO_LIMIT         3      // unanswered hellos before a connection is given up on, about 15 seconds
#define RESEND_TIMEOUT_MIN  200
#define RESEND_TIMEOUT_MAX  8000
#define RESEND_TIMEOUT_FIRST 1000  // until the round trip has been measured once
#define DUPLICATE_ACKS_LOST 3      // three acknowledgements of the same byte mean the next one was lost
#define INITIAL_SEGMENTS_IN_FLIGHT 10   // RFC 6928: what a link is assumed to carry before it proves otherwise
#define ACKNOWLEDGE_EVERY   2      // segments received before one goes back, RFC 1122

// One path, one size: there is a single tunnel per app, so what fits through it is the same for
// every stream. A measurement may lower it; nothing may raise it above what has been tested.
static int segmentMax = WG_TCP_SEGMENT_MAX;

int setTcpSegmentMax(int bytes)
{
   if (bytes < 536) bytes = 536;   // the smallest any end has to accept
   if (bytes > WG_TCP_SEGMENT_MAX) bytes = WG_TCP_SEGMENT_MAX;

   segmentMax = bytes;
   return segmentMax;
}

// sequence numbers count up and wrap round, so they are compared by difference rather than value
static int isAfter(uint32_t value, uint32_t reference)
{
   return (int32_t)(value - reference) > 0;
}

static int isAtOrAfter(uint32_t value, uint32_t reference)
{
   return (int32_t)(value - reference) >= 0;
}

static int getSmaller(int first, int second)
{
   return first < second ? first : second;
}

// the rings are indexed by sequence number, so a run of bytes is written where it belongs whatever
// order it arrived in. both sizes are powers of two, which is what makes the index a mask.
static void writeRing(uint8_t *ring, int size, uint32_t sequence, const uint8_t *data, int length)
{
   int offset = (int)(sequence & (uint32_t)(size - 1));
   int firstPart = getSmaller(size - offset, length);

   memCopy(ring + offset, data, firstPart);
   if (length > firstPart) memCopy(ring, data + firstPart, length - firstPart);
}

static void readRing(const uint8_t *ring, int size, uint32_t sequence, uint8_t *out, int length)
{
   int offset = (int)(sequence & (uint32_t)(size - 1));
   int firstPart = getSmaller(size - offset, length);

   memCopy(out, ring + offset, firstPart);
   if (length > firstPart) memCopy(out + firstPart, ring, length - firstPart);
}

static int getTcpSendRoom(const WgTcpConnection *connection)
{
   return WG_TCP_SEND_MAX - (int)(connection->sendEnd - connection->sendUnconfirmed);
}

// a goodbye takes a sequence number of its own without being a byte the app can read, so it is
// counted out again here
int getTcpReadable(const WgTcpConnection *connection)
{
   return (int)(connection->receiveNext - connection->receiveStart) - connection->remoteHasFinished;
}

// what we tell the other end it may send: whatever is free in the receive ring
static uint32_t getReceiveWindow(const WgTcpConnection *connection)
{
   return (uint32_t)(WG_TCP_RECEIVE_MAX - getTcpReadable(connection));
}

// TCP's checksum covers a few IP fields as well as its own header and data
static uint16_t getTcpChecksum(uint32_t sourceAddress, uint32_t destinationAddress, const uint8_t *segment,
                               int segmentLength)
{
   uint32_t sum = (sourceAddress >> 16) + (sourceAddress & 0xFFFF) + (destinationAddress >> 16) +
                  (destinationAddress & 0xFFFF) + IP_PROTOCOL_TCP + (uint32_t)segmentLength;

   for (int index = 0; index + 1 < segmentLength; index += 2) sum += load16be(segment + index);
   if (segmentLength & 1) sum += (uint32_t)segment[segmentLength - 1] << 8;

   while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
   return (uint16_t)~sum;
}

// A hello carries what the two ends have to agree before anything else: the largest segment we will
// take, and how far our advertised window is shifted. The window field is only 16 bits, so without
// the shift no window over 64 KB can be asked for, and speed is capped at that per round trip.
static int writeHelloOptions(uint8_t *options)
{
   options[0] = OPTION_SEGMENT_MAX;
   options[1] = 4;
   store16be(options + 2, (uint16_t)segmentMax);
   options[4] = OPTION_WINDOW_SHIFT;
   options[5] = 3;
   options[6] = WG_TCP_WINDOW_SHIFT;
   options[7] = OPTION_NOTHING;
   options[8] = OPTION_SACK_PERMITTED;   // so a single lost packet costs one packet, not a window
   options[9] = 2;
   options[10] = OPTION_NOTHING;
   options[11] = OPTION_NOTHING;   // pads the options out to a multiple of four
   return HELLO_OPTIONS_LENGTH;
}

// The runs that arrived after a gap, named so the other end resends only what is missing. Without
// this it can only see the gap and has to send everything after it a second time.
static int writeSackOption(uint8_t *options, const WgTcpConnection *connection)
{
   int blocks = connection->holeCount < SACK_BLOCK_MAX ? connection->holeCount : SACK_BLOCK_MAX;
   if (blocks <= 0) return 0;

   options[0] = OPTION_NOTHING;
   options[1] = OPTION_NOTHING;
   options[2] = OPTION_SACK;
   options[3] = (uint8_t)(2 + blocks * 8);

   for (int index = 0; index < blocks; index++) {
      store32be(options + 4 + index * 8, connection->holes[index].start);
      store32be(options + 8 + index * 8, connection->holes[index].end);
   }

   return 4 + blocks * 8;
}

// Build one segment at the given sequence number, taking its data from the send ring.
static int buildSegment(uint8_t *packet, int capacity, WgTcpConnection *connection, uint32_t localAddress,
                        uint8_t flags, uint32_t sequence, int dataLength)
{
   int isHello = (flags & TCP_FLAG_SYN) != 0;
   int wantsSacks = !isHello && dataLength == 0 && connection->peerTakesSacks && connection->holeCount > 0;

   uint8_t sackOption[4 + SACK_BLOCK_MAX * 8];
   int sackLength = wantsSacks ? writeSackOption(sackOption, connection) : 0;

   int optionsLength = isHello ? HELLO_OPTIONS_LENGTH : sackLength;
   int segmentLength = TCP_HEADER_LENGTH + optionsLength + dataLength;
   int totalLength = IPV4_HEADER_LENGTH + segmentLength;
   if (capacity < totalLength) return -1;

   // the IP header around it
   memSet(packet, 0, IPV4_HEADER_LENGTH);
   packet[0] = 0x45;
   store16be(packet + 2, (uint16_t)totalLength);
   packet[8] = 64;
   packet[9] = IP_PROTOCOL_TCP;
   store32be(packet + 12, localAddress);
   store32be(packet + 16, connection->remoteAddress);
   store16be(packet + 10, getInternetChecksum(packet, IPV4_HEADER_LENGTH));

   // the TCP header. the window in a hello is unscaled: the shift only applies once both ends have
   // agreed it, which is what the hello is doing, and only if they offered scaling as well.
   uint32_t window = getReceiveWindow(connection);
   if (!isHello) window >>= connection->localWindowShift;
   if (window > 0xFFFF) window = 0xFFFF;
   connection->advertisedWindow = window;

   uint8_t *segment = packet + IPV4_HEADER_LENGTH;
   memSet(segment, 0, TCP_HEADER_LENGTH);
   store16be(segment, connection->localPort);
   store16be(segment + 2, connection->remotePort);
   store32be(segment + 4, sequence);
   store32be(segment + 8, (flags & TCP_FLAG_ACK) ? connection->receiveNext : 0);
   segment[12] = (uint8_t)(((TCP_HEADER_LENGTH + optionsLength) / 4) << 4);
   segment[13] = flags;
   store16be(segment + 14, (uint16_t)window);

   if (isHello) writeHelloOptions(segment + TCP_HEADER_LENGTH);
   else if (sackLength > 0) memCopy(segment + TCP_HEADER_LENGTH, sackOption, sackLength);
   if (dataLength > 0)
      readRing(connection->outgoing, WG_TCP_SEND_MAX, sequence, segment + TCP_HEADER_LENGTH + optionsLength,
               dataLength);

   store16be(segment + 16, getTcpChecksum(localAddress, connection->remoteAddress, segment, segmentLength));

   // a hello and a goodbye each take a sequence number of their own, on top of any data
   uint32_t reached = sequence + (uint32_t)dataLength + ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1 : 0);
   if (isAfter(reached, connection->sendHighest)) connection->sendHighest = reached;

   return totalLength;
}

int buildTcpControl(uint8_t *packet, int capacity, WgTcpConnection *connection, uint32_t localAddress, uint8_t flags)
{
   // the goodbye sits one before sendNext, which is where closing counted it
   uint32_t sequence = (flags & TCP_FLAG_FIN) ? connection->sendNext - 1 : connection->sendNext;
   return buildSegment(packet, capacity, connection, localAddress, flags, sequence, 0);
}

// One byte of what is waiting, sent when they have offered no room at all. An acknowledgement on
// its own needs no answer, so a window update lost on the way back would stop the stream for good;
// a byte has to be answered (RFC 1122). It is not counted as sent, so it goes out again in order.
int buildTcpProbe(uint8_t *packet, int capacity, WgTcpConnection *connection, uint32_t localAddress)
{
   if (connection->sendEnd == connection->sendNext) return 0;
   return buildSegment(packet, capacity, connection, localAddress, TCP_FLAG_PSH | TCP_FLAG_ACK,
                       connection->sendNext, 1);
}

// They stop sending when the room we advertised runs out, and nothing but a new number starts them
// again, so room freed by reading has to be announced rather than waiting for a segment to carry
// it. Waiting until it is worth announcing keeps this from becoming one packet per read: RFC 1122
// asks for half the buffer, or two segments, whichever is smaller.
int shouldAnnounceWindow(const WgTcpConnection *connection)
{
   uint32_t announced = connection->advertisedWindow << connection->localWindowShift;
   uint32_t free = getReceiveWindow(connection);
   uint32_t worthSaying = (uint32_t)getSmaller(WG_TCP_RECEIVE_MAX / 2, 2 * connection->peerSegmentMax);

   return free >= announced + worthSaying;
}

int buildTcpData(uint8_t *packet, int capacity, WgTcpConnection *connection, uint32_t localAddress, uint64_t nowMs)
{
   if (connection->state != WG_TCP_OPEN) return 0;

   int inFlight = (int)(connection->sendNext - connection->sendUnconfirmed);
   int allowed = getSmaller((int)connection->peerWindow, connection->congestionWindow) - inFlight;
   int waiting = (int)(connection->sendEnd - connection->sendNext);
   int length = getSmaller(getSmaller(allowed, waiting), connection->peerSegmentMax);
   if (length <= 0) return 0;

   // a part-full segment waits while anything is unconfirmed, so a stream of small writes does not
   // become a stream of small packets (Nagle, RFC 896)
   if (length < connection->peerSegmentMax && inFlight > 0) return 0;

   int packetLength = buildSegment(packet, capacity, connection, localAddress, TCP_FLAG_PSH | TCP_FLAG_ACK,
                                   connection->sendNext, length);
   if (packetLength < 0) return 0;

   if (inFlight == 0) connection->oldestSentMs = nowMs;

   // time one segment at a time: enough to keep the resend timer honest, and Karn's rule says a
   // segment that had to be sent twice cannot be timed at all
   if (!connection->rttSamplePending) {
      connection->rttSamplePending = 1;
      connection->rttSampleSequence = connection->sendNext + (uint32_t)length;
      connection->rttSampleSentMs = nowMs;
   }

   connection->sendNext += (uint32_t)length;
   return packetLength;
}

static void failConnection(WgTcpConnection *connection, const char *reason)
{
   connection->state = WG_TCP_FAILED;
   strCopy(connection->failureReason, sizeof connection->failureReason, reason);
   logTrace(TAG "tcp: connection to port %d failed, %s\n", connection->remotePort, reason);
}

// back off after a loss: assume the link carries half what it was carrying, and start again
static void setSlowStartThreshold(WgTcpConnection *connection)
{
   int inFlight = (int)(connection->sendNext - connection->sendUnconfirmed);
   connection->slowStartThreshold = inFlight / 2;
   if (connection->slowStartThreshold < 2 * connection->peerSegmentMax)
      connection->slowStartThreshold = 2 * connection->peerSegmentMax;
}

int resendTcpFromOldest(WgTcpConnection *connection, uint64_t nowMs)
{
   if (++connection->resendCount > RESEND_LIMIT) {
      failConnection(connection, "the other end stopped confirming what we sent");
      return -1;
   }

   setSlowStartThreshold(connection);
   connection->congestionWindow = connection->peerSegmentMax;
   connection->sendNext = connection->sendUnconfirmed;   // everything unconfirmed goes out again
   connection->rttSamplePending = 0;
   connection->oldestSentMs = nowMs;

   // a timer that fired without an answer was too short for this link, so double it
   connection->resendTimeoutMs *= 2;
   if (connection->resendTimeoutMs > RESEND_TIMEOUT_MAX) connection->resendTimeoutMs = RESEND_TIMEOUT_MAX;
   return 0;
}

int resendTcpHello(WgTcpConnection *connection, uint64_t nowMs)
{
   if (++connection->resendCount > HELLO_LIMIT) {
      failConnection(connection, "the other end did not answer the hello");
      return -1;
   }

   connection->oldestSentMs = nowMs;
   connection->resendTimeoutMs *= 2;   // as for data: a timer that fired unanswered was too short
   if (connection->resendTimeoutMs > RESEND_TIMEOUT_MAX) connection->resendTimeoutMs = RESEND_TIMEOUT_MAX;
   return 0;
}

int writeTcpOutgoing(WgTcpConnection *connection, const uint8_t *data, int length)
{
   int take = getSmaller(length, getTcpSendRoom(connection));
   if (take <= 0) return 0;

   writeRing(connection->outgoing, WG_TCP_SEND_MAX, connection->sendEnd, data, take);
   connection->sendEnd += (uint32_t)take;
   return take;
}

int readTcpIncoming(WgTcpConnection *connection, uint8_t *buffer, int capacity)
{
   int take = getSmaller(capacity, getTcpReadable(connection));
   if (take <= 0) return 0;

   readRing(connection->incoming, WG_TCP_RECEIVE_MAX, connection->receiveStart, buffer, take);
   connection->receiveStart += (uint32_t)take;
   return take;
}

void openTcpConnection(WgTcpConnection *connection, uint32_t remoteAddress, uint16_t remotePort, uint16_t localPort,
                       uint32_t initialSequence)
{
   uint8_t *outgoing = connection->outgoing;
   uint8_t *incoming = connection->incoming;

   memSet(connection, 0, sizeof *connection);
   connection->outgoing = outgoing;
   connection->incoming = incoming;
   connection->state = WG_TCP_CONNECTING;
   connection->remoteAddress = remoteAddress;
   connection->remotePort = remotePort;
   connection->localPort = localPort;
   connection->sendNext = initialSequence;
   connection->sendUnconfirmed = initialSequence;
   connection->sendEnd = initialSequence;
   connection->sendHighest = initialSequence;

   // until the other end says otherwise: the smallest segment anything must accept, and a window
   // of one segment so the first data goes out without waiting
   connection->peerSegmentMax = 536;   // RFC 9293: what every end has to take without being told
   connection->peerWindow = 536;
   connection->congestionWindow = 536;
   connection->slowStartThreshold = WG_TCP_SEND_MAX;
   connection->resendTimeoutMs = RESEND_TIMEOUT_FIRST;
}

// read the options a hello carries. anything we do not recognise is stepped over.
static void readHelloOptions(const uint8_t *options, int length, TcpSegment *out)
{
   for (int offset = 0; offset < length;) {
      uint8_t kind = options[offset];
      if (kind == OPTION_END) return;
      if (kind == OPTION_NOTHING) { offset++; continue; }

      if (offset + 1 >= length) return;
      int optionLength = options[offset + 1];
      if (optionLength < 2 || offset + optionLength > length) return;

      if (kind == OPTION_SEGMENT_MAX && optionLength == 4) out->segmentMax = load16be(options + offset + 2);
      if (kind == OPTION_SACK_PERMITTED && optionLength == 2) out->takesSacks = 1;
      if (kind == OPTION_WINDOW_SHIFT && optionLength == 3) {
         out->hasWindowScale = 1;
         out->windowShift = options[offset + 2];
         if (out->windowShift > 14) out->windowShift = 14;   // RFC 7323 caps it here
      }

      offset += optionLength;
   }
}

int readTcpSegment(const uint8_t *packet, int length, const WgTcpConnection *connection, TcpSegment *out)
{
   if (length < IPV4_HEADER_LENGTH + TCP_HEADER_LENGTH) return -1;
   if ((packet[0] >> 4) != 4 || packet[9] != IP_PROTOCOL_TCP) return -1;

   int headerLength = (packet[0] & 0x0F) * 4;
   int totalLength = load16be(packet + 2);
   if (headerLength < IPV4_HEADER_LENGTH) return -1;
   if (totalLength > length || totalLength < headerLength + TCP_HEADER_LENGTH) return -1;

   const uint8_t *segment = packet + headerLength;
   if (load16be(segment) != connection->remotePort || load16be(segment + 2) != connection->localPort) return -1;
   if (load32be(packet + 12) != connection->remoteAddress) return -1;

   int segmentHeaderLength = (segment[12] >> 4) * 4;
   if (segmentHeaderLength < TCP_HEADER_LENGTH || headerLength + segmentHeaderLength > totalLength) return -1;

   // Everything inside the tunnel is authenticated, but between the VPN server and the far end it is
   // not, so this is what catches damage on that part of the path. Summing a correct segment with
   // its checksum field left in place gives zero.
   int segmentLength = totalLength - headerLength;
   if (getTcpChecksum(load32be(packet + 12), load32be(packet + 16), segment, segmentLength) != 0) {
      logTrace(TAG "tcp: a %d byte segment from port %d was damaged on the way\n", segmentLength, connection->remotePort);
      return -1;
   }

   memSet(out, 0, sizeof *out);
   out->flags = segment[13];
   out->sequence = load32be(segment + 4);
   out->acknowledgment = load32be(segment + 8);
   out->window = load16be(segment + 14);
   out->data = segment + segmentHeaderLength;
   out->dataLength = totalLength - headerLength - segmentHeaderLength;

   if (out->flags & TCP_FLAG_SYN)
      readHelloOptions(segment + TCP_HEADER_LENGTH, segmentHeaderLength - TCP_HEADER_LENGTH, out);

   return 0;
}

// Record a run of bytes that has arrived, joining it to any run it touches. Merged runs are what
// makes the rest simple: at most one can ever start where the next expected byte is. Returns 0 when
// there is no room left to remember it.
static int addReceivedRange(WgTcpConnection *connection, uint32_t start, uint32_t end)
{
   int index = 0;
   while (index < connection->holeCount) {
      WgSequenceRange *range = &connection->holes[index];
      if (isAfter(range->start, end) || isAfter(start, range->end)) { index++; continue; }

      if (isAfter(start, range->start)) start = range->start;
      if (isAfter(range->end, end)) end = range->end;
      connection->holes[index] = connection->holes[--connection->holeCount];
   }

   if (connection->holeCount >= WG_TCP_HOLE_MAX) return 0;

   connection->holes[connection->holeCount].start = start;
   connection->holes[connection->holeCount].end = end;
   connection->holeCount++;
   return 1;
}

// take every run that now starts where the next expected byte is: filling one gap can join up
// several runs that arrived earlier
static void absorbReceivedRange(WgTcpConnection *connection)
{
   for (int index = 0; index < connection->holeCount;) {
      if (connection->holes[index].start != connection->receiveNext) { index++; continue; }

      connection->receiveNext = connection->holes[index].end;
      connection->holes[index] = connection->holes[--connection->holeCount];
      index = 0;
   }
}

// fold arriving data into the receive ring, wherever in the stream it belongs
static void takeSegmentData(WgTcpConnection *connection, const TcpSegment *segment)
{
   uint32_t start = segment->sequence;
   uint32_t end = start + (uint32_t)segment->dataLength;

   // Bytes past the window we advertised are cut off first, then bytes we already have are dropped.
   // The order matters: a full ring makes the cut leave nothing, which is what has to happen when
   // they send anyway, and clipping afterwards would let it be written over unread data.
   uint32_t windowEnd = connection->receiveStart + WG_TCP_RECEIVE_MAX;
   if (isAfter(end, windowEnd)) end = windowEnd;
   if (isAtOrAfter(start, windowEnd)) return;
   if (isAtOrAfter(connection->receiveNext, end)) return;

   const uint8_t *data = segment->data;
   if (isAfter(connection->receiveNext, start)) {
      data += connection->receiveNext - start;
      start = connection->receiveNext;
   }

   // a run we cannot remember is dropped whole: writing it and then forgetting where it went would
   // leave bytes in the ring that nothing will ever hand to the app
   if (!addReceivedRange(connection, start, end)) return;

   writeRing(connection->incoming, WG_TCP_RECEIVE_MAX, start, data, (int)(end - start));
   absorbReceivedRange(connection);
}

// what the other end confirmed: release that much of the send ring and let the window grow
static void takeAcknowledgement(WgTcpConnection *connection, const TcpSegment *segment, uint64_t nowMs)
{
   int inFlight = (int)(connection->sendNext - connection->sendUnconfirmed);

   // The same acknowledgement repeated, carrying nothing and changing nothing, means they are still
   // waiting for one particular packet. One that opens their window is telling us something else.
   uint32_t offeredWindow = (uint32_t)segment->window << connection->peerWindowShift;
   if (segment->acknowledgment == connection->sendUnconfirmed && segment->dataLength == 0 && inFlight > 0 &&
       offeredWindow == connection->peerWindow) {
      if (++connection->duplicateAcks != DUPLICATE_ACKS_LOST) return;

      setSlowStartThreshold(connection);
      connection->congestionWindow = connection->slowStartThreshold;
      connection->sendNext = connection->sendUnconfirmed;
      connection->rttSamplePending = 0;
      connection->oldestSentMs = nowMs;
      return;
   }

   if (!isAfter(segment->acknowledgment, connection->sendUnconfirmed)) return;
   if (isAfter(segment->acknowledgment, connection->sendHighest)) return;   // confirming what we never sent

   int confirmed = (int)(segment->acknowledgment - connection->sendUnconfirmed);
   connection->sendUnconfirmed = segment->acknowledgment;

   // a resend winds the cursor back, so a confirmation can arrive for data that is now ahead of it
   if (isAfter(connection->sendUnconfirmed, connection->sendNext)) connection->sendNext = connection->sendUnconfirmed;

   connection->duplicateAcks = 0;
   connection->resendCount = 0;
   connection->oldestSentMs = nowMs;

   // the link carried it, so allow more next time: quickly at first, then one segment per round
   // trip once the window is large enough to be worth being careful with
   if (connection->congestionWindow < connection->slowStartThreshold)
      connection->congestionWindow += getSmaller(confirmed, connection->peerSegmentMax);
   else
      connection->congestionWindow += connection->peerSegmentMax * connection->peerSegmentMax /
                                      connection->congestionWindow;
   if (connection->congestionWindow > WG_TCP_SEND_MAX) connection->congestionWindow = WG_TCP_SEND_MAX;

   // the round trip, smoothed, RFC 6298
   if (connection->rttSamplePending && isAtOrAfter(segment->acknowledgment, connection->rttSampleSequence)) {
      int sample = (int)(nowMs - connection->rttSampleSentMs);
      if (connection->smoothedRttMs == 0) {
         connection->smoothedRttMs = sample;
         connection->rttVarianceMs = sample / 2;
      } else {
         int difference = sample - connection->smoothedRttMs;
         if (difference < 0) difference = -difference;
         connection->rttVarianceMs = (3 * connection->rttVarianceMs + difference) / 4;
         connection->smoothedRttMs = (7 * connection->smoothedRttMs + sample) / 8;
      }

      connection->resendTimeoutMs = connection->smoothedRttMs + 4 * connection->rttVarianceMs;
      if (connection->resendTimeoutMs < RESEND_TIMEOUT_MIN) connection->resendTimeoutMs = RESEND_TIMEOUT_MIN;
      if (connection->resendTimeoutMs > RESEND_TIMEOUT_MAX) connection->resendTimeoutMs = RESEND_TIMEOUT_MAX;
      connection->rttSamplePending = 0;
   }
}

TcpReaction processTcpSegment(WgTcpConnection *connection, const TcpSegment *segment, uint64_t nowMs)
{
   // A refusal is believed in two places only: one that answers our hello, and afterwards one whose
   // sequence number falls inside the window we advertised. Between the VPN server and the far end
   // nothing is authenticated, so any other one is a stray that would kill a working stream.
   if (segment->flags & TCP_FLAG_RST) {
      uint32_t windowEnd = connection->receiveStart + WG_TCP_RECEIVE_MAX;
      int answersHello = connection->state == WG_TCP_CONNECTING && (segment->flags & TCP_FLAG_ACK) &&
                         segment->acknowledgment == connection->sendNext + 1;
      int isInWindow = connection->state != WG_TCP_CONNECTING &&
                       isAtOrAfter(segment->sequence, connection->receiveNext) && isAfter(windowEnd, segment->sequence);
      if (!answersHello && !isInWindow) return TCP_REACT_NOTHING;

      // one that arrives after we have said goodbye is how an end that still had data to send
      // finishes the conversation, so it ends the stream without being a failure
      if (connection->state == WG_TCP_CLOSING) {
         connection->state = WG_TCP_CLOSED;
         return TCP_REACT_NOTHING;
      }

      failConnection(connection, "the other end refused it");
      return TCP_REACT_NOTHING;
   }

   // the answer to our hello: their numbering starts here, and what they will take is now known
   if (connection->state == WG_TCP_CONNECTING) {
      if ((segment->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) != (TCP_FLAG_SYN | TCP_FLAG_ACK)) return TCP_REACT_NOTHING;
      if (segment->acknowledgment != connection->sendNext + 1) return TCP_REACT_NOTHING;

      connection->sendNext++;
      connection->sendUnconfirmed = connection->sendNext;
      connection->sendEnd = connection->sendNext;
      connection->receiveNext = segment->sequence + 1;
      connection->receiveStart = connection->receiveNext;

      // RFC 7323: our own window may only be shifted if their hello offered scaling as well
      connection->peerWindowShift = segment->hasWindowScale ? segment->windowShift : 0;
      connection->localWindowShift = segment->hasWindowScale ? WG_TCP_WINDOW_SHIFT : 0;
      connection->peerWindow = (uint32_t)segment->window << connection->peerWindowShift;
      connection->peerTakesSacks = segment->takesSacks;
      if (segment->segmentMax >= 536) connection->peerSegmentMax = getSmaller(segment->segmentMax, segmentMax);
      connection->congestionWindow = connection->peerSegmentMax * INITIAL_SEGMENTS_IN_FLIGHT;

      connection->state = WG_TCP_OPEN;
      return TCP_REACT_ACKNOWLEDGE;
   }

   if (connection->state != WG_TCP_OPEN && connection->state != WG_TCP_CLOSING) return TCP_REACT_NOTHING;

   if (segment->flags & TCP_FLAG_ACK) {
      takeAcknowledgement(connection, segment, nowMs);

      // one that overtook a newer one would otherwise shrink the window back to what it was
      if (isAtOrAfter(segment->acknowledgment, connection->sendUnconfirmed))
         connection->peerWindow = (uint32_t)segment->window << connection->peerWindowShift;
   }

   TcpReaction reaction = TCP_REACT_NOTHING;

   if (segment->dataLength > 0) {
      int wasInOrder = segment->sequence == connection->receiveNext;
      takeSegmentData(connection, segment);

      // a gap is reported at once, so they resend without waiting for their own timer. otherwise
      // one acknowledgement covers two segments, which halves the packets going back.
      connection->segmentsSinceAcknowledgement++;
      if (!wasInOrder || connection->segmentsSinceAcknowledgement >= ACKNOWLEDGE_EVERY)
         reaction = TCP_REACT_ACKNOWLEDGE;
   }

   // They have finished sending, but only once everything before the goodbye has arrived. The
   // goodbye's own sequence number is past the data it travels with, which is where a server puts
   // it when it sends the last of a page and closes in one packet.
   if ((segment->flags & TCP_FLAG_FIN) &&
       segment->sequence + (uint32_t)segment->dataLength == connection->receiveNext) {
      connection->receiveNext++;
      connection->remoteHasFinished = 1;
      reaction = TCP_REACT_ACKNOWLEDGE;
      if (connection->state == WG_TCP_CLOSING) connection->state = WG_TCP_CLOSED;
   }

   return reaction;
}
