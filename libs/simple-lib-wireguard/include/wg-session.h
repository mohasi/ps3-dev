#pragma once

// Once the handshake has agreed keys, packets travel as transport data messages: a 16 byte header
// followed by the encrypted packet and its tag (whitepaper section 5.4.6).

#include <stdint.h>

#include "wg-handshake.h"
#include "wg-replay.h"

#define WG_MESSAGE_DATA   4
#define WG_DATA_OVERHEAD  32   // 16 byte header plus the 16 byte tag, before padding

typedef struct {
   uint8_t  sendKey[32];
   uint8_t  receiveKey[32];
   uint32_t senderIndex;     // ours, the index the server puts on packets to us
   uint32_t receiverIndex;   // theirs, the index we put on packets to them
   uint64_t sendCounter;
   WgReplayWindow received;
} WgSession;

void startWgSession(WgSession *session, const WgHandshake *handshake);

// wrap one IP packet. returns the message length written to out, or -1 if out is too small.
int sealWgPacket(WgSession *session, const uint8_t *packet, int length, uint8_t *out, int outCapacity);

// unwrap one received message. returns the packet length written to out, 0 for a keepalive (which
// carries no packet), or -1 when the message is not for us or does not decrypt.
int openWgPacket(WgSession *session, const uint8_t *message, int length, uint8_t *out, int outCapacity);
