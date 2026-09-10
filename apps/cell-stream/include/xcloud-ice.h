#pragma once

// xcloud-ice - the console's half of working out how the two ends will reach each other.
//
// The connection description names credentials that later prove each side is who it said it was, so
// they are generated fresh per session and never reused. Addresses are traded separately from the
// description itself, which is why this is two steps rather than one.

#define ICE_USERNAME_MAX 8
#define ICE_PWD_MAX   32
#define ICE_ADDRESS_MAX 64

// Fresh credentials and a UDP socket to receive on. Returns 0, or -1 if the console would not give
// randomness or a socket.
int openXcloudIce(void);
void closeXcloudIce(void);

// The description offered to the machine, built around the credentials above. Points at a buffer
// owned here.
const char *getXcloudOffer(void);

// Tells the machine where to reach us, then collects where it can be reached. Returns how many of
// the machine's addresses came back, or -1.
int exchangeXcloudIceCandidates(void);

// Reads the credentials the machine chose out of its answer, so our checks can be addressed to it.
// 0 on success, -1 when the answer did not carry them.
int readXcloudRemoteCredentials(const char *answerSdp);

// Sends checks to the machine and answers the ones it sends back, until both directions have
// worked. A connection is only usable once each end has proved itself to the other. Returns 0 when
// both have, or -1.
int negotiateXcloudConnection(void);

// Answers one connectivity check. The machine keeps checking for as long as the connection lives,
// and gives up on an end that stops answering, so these must keep being serviced alongside
// whatever else the socket is carrying.
void answerXcloudCheck(const void *packet, int length);

// 1 when a packet is one of those checks rather than encrypted traffic or media. The three are
// told apart by their first byte, which the standards keep in separate ranges for this purpose.
static inline int isXcloudCheck(const void *packet, int length)
{
   return length > 0 && *(const unsigned char *)packet <= 3;
}

// What the machine said its certificate would be, from the same answer. Empty until then.
const char *getXcloudRemoteFingerprint(void);

// Once a connection is agreed, everything after it travels on the same socket to the same address.
// Both return how many bytes moved, or -1. A receive that times out returns -1 as well: at this
// level nothing has arrived either way.
int sendXcloudDatagram(const void *data, int length);
int receiveXcloudDatagram(void *buffer, int capacity, int timeoutMs);
