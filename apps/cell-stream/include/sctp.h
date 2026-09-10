#pragma once

// sctp - the channel the machine sends its messages on and the console sends button presses back.
//
// It runs inside the encrypted connection, and adds what that does not give: many separate
// channels over one connection, each message kept whole, and lost messages sent again. Only the
// part of it a game stream uses is here.
//
// This knows nothing about Xbox. It is given whole messages to send and hands back whole messages
// that arrived, and the caller decides what they mean.

#include <stdint.h>

// what a message is: text, bytes, or one of the channel's own control messages
typedef enum {
   SCTP_PAYLOAD_CONTROL = 50,
   SCTP_PAYLOAD_TEXT    = 51,
   SCTP_PAYLOAD_BINARY  = 53
} SctpPayloadKind;

// how a packet leaves. returns what was sent, or -1.
typedef int (*SctpSendFunc)(const void *packet, int length);

// Starts from nothing, ready for the machine to open the association. 0 or -1.
int openSctp(SctpSendFunc send);

typedef struct {
   int channel;               // which of the separate channels it arrived on
   SctpPayloadKind kind;
   const uint8_t *data;
   int length;
} SctpMessage;

// the most messages one packet is reported as carrying; a caller asking for more gets this many
#define SCTP_MESSAGES_MAX 4

// Takes one arriving packet, answers whatever it owes, and reports the messages it carried into
// messages, up to capacity of them, never more than SCTP_MESSAGES_MAX. Returns how many, or -1 if
// the packet was not genuine. The data each points at stays valid until the next call.
int feedSctp(const void *packet, int length, SctpMessage *messages, int capacity);

// 1 once the machine and the console have agreed to talk.
int isSctpOpen(void);

// Names one channel, so both ends know what it carries. The far end answers, after which the
// channel can be used. ordered says whether messages must arrive in the order they were sent;
// where they need not, a message is given up on rather than sent again, which suits button
// presses that are stale by the time a second attempt arrives. 0 or -1.
//
// This naming is a small protocol of its own carried on the channel it names. It lives here
// because it is two messages long and useless on its own.
int openSctpChannel(int channel, const char *label, const char *protocol, int ordered);

// 1 once the far end has answered for that channel.
int isSctpChannelOpen(int channel);

// Sends one whole message on a channel. 0 or -1.
int sendSctpMessage(int channel, SctpPayloadKind kind, const void *data, int length);

// Checks the checksum against its published test value. Nothing else here can be trusted if this
// fails, and it is cheap, so it runs once at startup.
void runSctpSelfTest(void);
