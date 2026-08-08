#include "wg-session.h"

#include "chacha20-poly1305.h"
#include "dbg.h"
#include "string-utilities.h"
#include "wg-bytes.h"

#define TAG "[wg] "

#define DATA_RECEIVER_INDEX 4
#define DATA_COUNTER        8
#define DATA_PAYLOAD        16

void startWgSession(WgSession *session, const WgHandshake *handshake)
{
   memSet(session, 0, sizeof *session);
   memCopy(session->sendKey, handshake->sendKey, 32);
   memCopy(session->receiveKey, handshake->receiveKey, 32);
   session->senderIndex = handshake->senderIndex;
   session->receiverIndex = handshake->receiverIndex;
}

int sealWgPacket(WgSession *session, const uint8_t *packet, int length, uint8_t *out, int outCapacity)
{
   // packets are padded to a multiple of 16 bytes so their length leaks less about the contents
   int paddedLength = (length + 15) & ~15;
   if (outCapacity < DATA_PAYLOAD + paddedLength + AEAD_TAG_LENGTH) {
      logError(TAG "session: send buffer too small for a %d byte packet\n", length);
      return -1;
   }

   memSet(out, 0, DATA_PAYLOAD);
   out[0] = WG_MESSAGE_DATA;
   store32le(out + DATA_RECEIVER_INDEX, session->receiverIndex);
   store64le(out + DATA_COUNTER, session->sendCounter);

   uint8_t padded[1536];
   if (paddedLength > (int)sizeof padded) {
      logError(TAG "session: packet of %d bytes is over the limit\n", length);
      return -1;
   }
   memCopy(padded, packet, length);
   memSet(padded + length, 0, paddedLength - length);

   uint8_t nonce[AEAD_NONCE_LENGTH];
   storeWgNonce(nonce, session->sendCounter);
   sealChaCha20Poly1305(out + DATA_PAYLOAD, padded, paddedLength, 0, 0, nonce, session->sendKey);
   session->sendCounter++;

   memSet(padded, 0, paddedLength);   // only what was written holds anything to wipe
   return DATA_PAYLOAD + paddedLength + AEAD_TAG_LENGTH;
}

int openWgPacket(WgSession *session, const uint8_t *message, int length, uint8_t *out, int outCapacity)
{
   if (length < DATA_PAYLOAD + AEAD_TAG_LENGTH) {
      logError(TAG "session: message too short, %d bytes\n", length);
      return -1;
   }
   if (message[0] != WG_MESSAGE_DATA) {
      logError(TAG "session: not a data message, type %d\n", message[0]);
      return -1;
   }

   uint32_t receiverIndex = load32le(message + DATA_RECEIVER_INDEX);
   if (receiverIndex != session->senderIndex) {
      logError(TAG "session: message for index 0x%x, ours is 0x%x\n", receiverIndex, session->senderIndex);
      return -1;
   }

   int sealedLength = length - DATA_PAYLOAD;
   if (sealedLength - AEAD_TAG_LENGTH > outCapacity) {
      logError(TAG "session: receive buffer too small for %d bytes\n", sealedLength - AEAD_TAG_LENGTH);
      return -1;
   }

   uint64_t counter = load64le(message + DATA_COUNTER);

   uint8_t nonce[AEAD_NONCE_LENGTH];
   storeWgNonce(nonce, counter);
   if (!openChaCha20Poly1305(out, message + DATA_PAYLOAD, sealedLength, 0, 0, nonce, session->receiveKey)) {
      logError(TAG "session: message did not decrypt\n");
      return -1;
   }

   // only after it decrypts, so a forged counter cannot move the window
   if (!acceptWgCounter(&session->received, counter)) {
      logWarn(TAG "session: counter %llu has already been seen, packet dropped\n", (unsigned long long)counter);
      return -1;
   }

   return sealedLength - AEAD_TAG_LENGTH;   // 0 means a keepalive, which carries no packet
}
