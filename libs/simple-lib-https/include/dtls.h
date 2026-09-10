#pragma once

// dtls - the encrypted half of a peer-to-peer connection, run over datagrams instead of a stream.
//
// This is the same TLS 1.2 exchange the rest of this library does over a socket, with the parts
// that a stream normally provides added back: every message carries its own sequence number,
// messages can arrive out of order or not at all, and anything unanswered has to be sent again.
// BearSSL does not do this, so the exchange is written here and its cryptography borrowed.
//
// The caller keeps the socket. The same socket carries the connectivity checks and later the media,
// and only the caller can tell those apart, so it hands each arriving datagram over and provides a
// way to send.

#include <stdint.h>

// how a datagram gets out. returns what was sent, or -1.
typedef int (*DtlsSendFunc)(const void *datagram, int length);

// 1 when a datagram belongs to the encrypted exchange rather than to a connectivity check or to
// media. The three are told apart by their first byte, which the standards deliberately keep in
// separate ranges for exactly this.
static inline int isDtlsDatagram(const void *datagram, int length)
{
   if (length <= 0) return 0;
   uint8_t first = *(const uint8_t *)datagram;
   return first >= 20 && first <= 63;
}

// Begins the exchange as the side that speaks first, and sends the opening message. Our
// certificate must already exist, see dtls-certificate.h.
//
// peerFingerprint is what the connection description said the far end's certificate would be, in
// the colon-separated form it is written there. Nothing else vouches for the far end, so the
// exchange is abandoned if what arrives does not match it. 0 or -1.
int startDtls(DtlsSendFunc send, const char *peerFingerprint);

typedef enum {
   DTLS_HANDSHAKE_RUNNING,
   DTLS_HANDSHAKE_DONE,
   DTLS_HANDSHAKE_FAILED
} DtlsProgress;

// Hands over one arriving datagram and carries the exchange forward, sending whatever it owes in
// reply.
DtlsProgress feedDtls(const void *datagram, int length);

// Sends again whatever has not been answered yet. Datagrams go missing, and nothing underneath
// this notices, so the caller does this when a reply is overdue.
void resendDtls(void);

// Once the exchange is done, the connection carries ordinary content both ways.
//
// Reading takes one arriving datagram and writes what it held into out, returning how many bytes
// that was, 0 if the datagram carried nothing for the caller, or -1 if it was not genuine.
int readDtlsData(const void *datagram, int length, uint8_t *out, int capacity);
int sendDtlsData(const void *data, int length);

// The media is not carried inside this connection, only keyed by it: the exchange above agrees the
// keys, and the media then travels beside it on the same socket under its own cipher. These are
// those keys, valid once the exchange is done, laid out as the standard orders them: our key, the
// far end's key, our salt, the far end's salt.
#define DTLS_MEDIA_KEY_SIZE  16
#define DTLS_MEDIA_SALT_SIZE 14

const uint8_t *getDtlsMediaKeys(void);
