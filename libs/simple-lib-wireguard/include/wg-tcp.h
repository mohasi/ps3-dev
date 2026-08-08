#pragma once

// TCP inside the tunnel. Not for apps: they see connect, send, receive and close in wg-net.h, and
// never touch a segment or a connection.
//
// UDP is one packet, sent and forgotten. TCP is a conversation: both ends agree to start, number
// every byte so nothing arrives twice or out of order, confirm what they received, resend what got
// lost, and agree to stop. All of that has to be built here, because the console's own TCP talks to
// the network card and cannot be pointed at a tunnel.
//
// It is built to carry bulk data, not just requests. Data waiting to go out and data that has
// arrived both sit in rings indexed by their own sequence numbers, so a run that arrives early is
// simply written where it belongs and waits for the gap in front of it to fill.

#include <stdint.h>

// The largest chunk of data in one packet, and the ceiling on what a measurement may set. Around it
// go 20 bytes of IP and 20 of TCP inside the tunnel, then 16 of WireGuard header, padding up to a
// multiple of 16, a 16 byte tag and 28 bytes of outer IP and UDP: 1468 bytes on the wire. Measured
// on this console against ProtonVPN, 1468 got through and larger sizes started being split up.
#define WG_TCP_SEGMENT_MAX 1360

// How much data may be in flight in each direction. Speed is this divided by the round trip, so
// 256 KB against a 68 ms round trip is about 3.7 MB/s on one stream. Both must be powers of two:
// the rings are indexed by sequence number.
#define WG_TCP_RECEIVE_MAX 262144
#define WG_TCP_SEND_MAX    131072
#define WG_TCP_BUFFER_MAX  (WG_TCP_SEND_MAX + WG_TCP_RECEIVE_MAX)   // one allocation holds both

// Runs of data held while waiting for what comes before them. A 256 KB window is around 190
// packets, and with several streams sharing a link the gaps come in numbers: measured with eight
// slots, the list stayed full and every stream stopped, because a run that cannot be remembered has
// to be sent again and is then dropped again. Sixty four slots cost half a kilobyte per stream.
#define WG_TCP_HOLE_MAX 64
#define WG_TCP_WINDOW_SHIFT 7   // RFC 7323 scaling, without which no window over 64 KB can be asked for

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

typedef enum {
   WG_TCP_CLOSED,
   WG_TCP_CONNECTING,   // we have said hello and are waiting for the answer
   WG_TCP_OPEN,         // data may flow
   WG_TCP_CLOSING,      // we have said goodbye and are waiting for the other end to agree
   WG_TCP_FAILED
} WgTcpState;

typedef struct {
   uint32_t start, end;
} WgSequenceRange;

typedef struct {
   WgTcpState state;
   uint32_t   remoteAddress;
   uint16_t   remotePort;
   uint16_t   localPort;

   // both rings live in one allocation, taken when the stream opens and given back when it closes.
   // outgoing points at its start, which is what frees it.
   uint8_t *outgoing;
   uint8_t *incoming;

   // sending: [sendUnconfirmed, sendEnd) is held, [sendUnconfirmed, sendNext) is on its way
   uint32_t sendUnconfirmed;
   uint32_t sendNext;
   uint32_t sendEnd;
   uint32_t sendHighest;       // the furthest we have ever sent: a resend winds sendNext back, this stays
   uint64_t oldestSentMs;      // when the oldest unconfirmed byte went out, for the resend timer
   int      duplicateAcks;     // three in a row means the packet after them was lost
   int      resendCount;

   // what the other end and the link between us will take
   uint32_t peerWindow;
   int      peerWindowShift;
   int      peerSegmentMax;
   int      congestionWindow;      // what the link has proved it will carry without loss
   int      slowStartThreshold;

   // the round trip, measured, so the resend timer fits the link instead of guessing
   int      smoothedRttMs;
   int      rttVarianceMs;
   int      resendTimeoutMs;
   uint32_t rttSampleSequence;
   uint64_t rttSampleSentMs;
   int      rttSamplePending;

   // receiving: [receiveStart, receiveNext) is in order and unread, the holes are early arrivals
   uint32_t receiveStart;
   uint32_t receiveNext;
   WgSequenceRange holes[WG_TCP_HOLE_MAX];
   int      holeCount;
   int      segmentsSinceAcknowledgement;
   int      peerTakesSacks;     // it said in its hello that it can read a list of what arrived
   uint32_t advertisedWindow;   // what we last told them, so a reopened window is announced
   int      localWindowShift;   // how far our own window is shifted: 0 unless they offered scaling too

   int  remoteHasFinished;
   char failureReason[64];
} WgTcpConnection;

typedef struct {
   uint8_t        flags;
   uint32_t       sequence;
   uint32_t       acknowledgment;
   uint16_t       window;
   const uint8_t *data;
   int            dataLength;

   // only meaningful on a hello, which is the one place these can be agreed
   int segmentMax;
   int windowShift;
   int hasWindowScale;
   int takesSacks;
} TcpSegment;

typedef enum {
   TCP_REACT_NOTHING,
   TCP_REACT_ACKNOWLEDGE
} TcpReaction;

// The largest chunk of data our segments carry, returning the size actually taken. Lowered by a
// path that will not take a full sized packet; it can never go above WG_TCP_SEGMENT_MAX, which is
// the size this console has been tested with. It applies to streams opened afterwards.
int setTcpSegmentMax(int bytes);

// the rings must already point at WG_TCP_BUFFER_MAX bytes of storage; everything else is set here
void openTcpConnection(WgTcpConnection *connection, uint32_t remoteAddress, uint16_t remotePort, uint16_t localPort,
                       uint32_t initialSequence);

// -1 when the packet is not a segment of this connection, or its checksum says it was damaged
int readTcpSegment(const uint8_t *packet, int length, const WgTcpConnection *connection, TcpSegment *out);

TcpReaction processTcpSegment(WgTcpConnection *connection, const TcpSegment *segment, uint64_t nowMs);

// Build a segment carrying no data: a hello, an acknowledgement or a goodbye.
int buildTcpControl(uint8_t *packet, int capacity, WgTcpConnection *connection, uint32_t localAddress, uint8_t flags);

// Build the next segment of waiting data that the other end and the link will both take, and count
// it as sent. Returns its length, or 0 when nothing may go out yet.
//
// A segment that then fails to reach the tunnel is treated as a lost packet: the resend timer picks
// it up like any other.
int buildTcpData(uint8_t *packet, int capacity, WgTcpConnection *connection, uint32_t localAddress, uint64_t nowMs);

// Give up on what is in flight and send it again from the oldest unconfirmed byte. Returns -1 once
// the connection has been retried too many times and is marked failed.
int resendTcpFromOldest(WgTcpConnection *connection, uint64_t nowMs);

// Count one more unanswered hello and arm the timer for the next. Returns -1 once there have been
// enough of them and the connection is marked failed.
int resendTcpHello(WgTcpConnection *connection, uint64_t nowMs);

// Build one byte of waiting data to prod an end that has offered no room. Returns its length, or 0
// when there is nothing waiting. The byte is not counted as sent.
int buildTcpProbe(uint8_t *packet, int capacity, WgTcpConnection *connection, uint32_t localAddress);

// has enough room been freed that they should be told before the next segment carries it?
int shouldAnnounceWindow(const WgTcpConnection *connection);

// how much the app may still read
int getTcpReadable(const WgTcpConnection *connection);

int writeTcpOutgoing(WgTcpConnection *connection, const uint8_t *data, int length);
int readTcpIncoming(WgTcpConnection *connection, uint8_t *buffer, int capacity);
