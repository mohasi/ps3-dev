#pragma once

// srtp - the cipher the picture and sound travel under.
//
// The media does not go inside the encrypted connection: it travels beside it, on the same socket,
// under its own cipher, using keys the connection agreed. That is done so a lost or late packet
// costs one frame rather than stalling everything behind it, which is what a connection that
// insists on order would do.
//
// The console sends no media. It does send reports on what arrived, which travel the same way and
// are protected under their own keys, so that half is here too.

#include <stdint.h>

// 1 when a packet carries picture or sound rather than a report about the stream. The two share
// the socket, are shaped differently, and are protected differently, so they must be told apart
// before either is unwrapped. The second byte says which: the range 64 to 95 is reserved for
// reports precisely so this test is possible.
static inline int isSrtpMedia(const void *packet, int length)
{
   if (length < 2) return 0;
   int kind = ((const unsigned char *)packet)[1] & 0x7F;
   return kind < 64 || kind > 95;
}

// Where the contents of a media packet start. The header is not a fixed size: it grows if the
// packet credits other senders, and again if it carries extra fields, which a connection like
// this one always does. Assuming twelve bytes feeds those extra fields to the decoder as if they
// were picture. Returns -1 if the packet is too short to make sense of.
int getRtpPayloadOffset(const void *packet, int length);

// Sets up with the far end's key and salt, which come from getDtlsMediaKeys. 0 or -1.
int openSrtp(const uint8_t *key, const uint8_t *salt);

// Checks the key working-out against the published example. Everything else here is worthless if
// this is wrong, and it costs three cipher runs, so it runs once at startup.
void runSrtpSelfTest(void);

// Unwraps one arriving packet where it lies. Returns its length with the protection removed, or -1
// if it was altered or is not genuine. The header is left alone: the numbering in it is what puts
// the picture back in order.
int readSrtpPacket(uint8_t *packet, int length);

// Sets up the outgoing half with this console's own key and salt, the other pair from
// getDtlsMediaKeys. Needed only to send reports. 0 or -1.
int openSrtcpSend(const uint8_t *key, const uint8_t *salt);

// How much longer a wrapped report is than the report itself, so the caller can leave room.
#define SRTCP_OVERHEAD 14

// Sets up reading the reports that arrive, with the far end's key and salt, the same pair openSrtp
// takes. Separate because a report is protected differently from the picture. 0 or -1.
int openSrtcpRead(const uint8_t *key, const uint8_t *salt);

// Unwraps one arriving report where it lies. Returns its length with the protection removed, or -1.
int readSrtcpPacket(uint8_t *packet, int length);

// Wraps one report in place, ready to send. Returns the length to send, or -1 if there is not
// SRTCP_OVERHEAD spare in the buffer.
int writeSrtcpPacket(uint8_t *packet, int length, int capacity);
