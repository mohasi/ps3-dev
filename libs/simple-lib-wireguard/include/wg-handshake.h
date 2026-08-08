#pragma once

// The WireGuard handshake, initiator side. One exchange: we build an initiation message, the
// server answers with a response, and both ends end up holding the same pair of transport keys.
//
// The protocol is Noise IKpsk2 as described in the WireGuard whitepaper section 5.4. Everything
// here follows that document: the message layouts, the order the hash and chaining key are mixed,
// and the labels.

#include <stdint.h>

#include "wg-config.h"

#define WG_INITIATION_LENGTH 148
#define WG_RESPONSE_LENGTH   92

#define WG_MESSAGE_INITIATION 1
#define WG_MESSAGE_RESPONSE   2
#define WG_MESSAGE_COOKIE     3

typedef struct {
   uint8_t chainKey[32];
   uint8_t hash[32];
   uint8_t ephemeralPrivate[32];
   uint8_t ephemeralPublic[32];
   uint8_t staticPrivate[32];
   uint8_t staticPublic[32];
   uint8_t peerPublicKey[32];
   uint8_t peerEphemeral[32];   // only used when answering a handshake the server started
   uint8_t presharedKey[32];

   uint32_t senderIndex;        // the index we chose, echoed back by the server
   uint32_t receiverIndex;      // the index the server chose

   uint8_t sendKey[32];         // valid once the handshake completes, whichever side started it
   uint8_t receiveKey[32];
} WgHandshake;

// the authenticator carried by every handshake message, keyed by the receiver's public key. it
// covers everything in the message before the mac1 field itself.
void computeWgMac1(uint8_t mac[16], const uint8_t receiverPublicKey[32], const uint8_t *message, int length);

// build the initiation message. returns 0, or -1 if the random source failed.
int createWgInitiation(WgHandshake *handshake, const WgConfig *config, uint8_t message[WG_INITIATION_LENGTH]);

// consume the server's response and derive the transport keys. returns 0 on success, or -1 with
// the reason logged: wrong length, wrong type, wrong index, bad authenticator, or a payload that
// would not decrypt.
int processWgResponse(WgHandshake *handshake, const uint8_t *message, int length);

// The other direction: the server may start a handshake of its own, and a peer that ignores those
// loses the ability to receive anything the server wants to start sending.
//
// processWgInitiation checks the message and works out the shared state; createWgResponse then
// builds the reply and derives the transport keys. lastTimestamp holds the newest timestamp seen
// from this peer and is updated on success, which is what stops an old initiation being replayed.
int processWgInitiation(WgHandshake *handshake, const WgConfig *config, const uint8_t *message, int length,
                        uint8_t lastTimestamp[12]);
int createWgResponse(WgHandshake *handshake, uint8_t message[WG_RESPONSE_LENGTH]);
