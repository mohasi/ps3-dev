#pragma once

// Building and reading the IP packets that travel inside the tunnel.
//
// There is no IP stack in here. The PS3's own stack talks to the network card and cannot be told
// about a tunnel, so packets that go through the tunnel have to be assembled by hand. This is the
// smallest amount of that work which is useful: IPv4 carrying UDP, which covers DNS and, later, a
// torrent client's tracker and peer discovery traffic.

#include <stdint.h>

#define IPV4_HEADER_LENGTH 20
#define UDP_HEADER_LENGTH  8

// the checksum used by both IP and UDP (RFC 1071). a receiver checking a correct header gets zero.
uint16_t getInternetChecksum(const uint8_t *data, int length);

// write an IPv4 packet carrying a UDP payload. returns the packet length, or -1 if it will not fit.
int buildUdpPacket(uint8_t *packet, int capacity, uint32_t sourceAddress, uint32_t destinationAddress,
                   uint16_t sourcePort, uint16_t destinationPort, const uint8_t *payload, int payloadLength);

// Build a ping request with payloadLength bytes of filler. Used to find out how large a packet
// the path will carry: the largest size that comes back is the limit. Returns the packet length,
// or -1 if it will not fit.
int buildPingRequest(uint8_t *packet, int capacity, uint32_t sourceAddress, uint32_t destinationAddress,
                     uint16_t identifier, uint16_t sequence, int payloadLength);

// is this the reply to a ping we sent with that identifier and sequence number?
int isPingReplyFor(const uint8_t *packet, int length, uint16_t identifier, uint16_t sequence);

// Turn a received ping request into the reply to it, in place. Answering pings is ordinary host
// behaviour: the VPN gateway uses them to check we are still there, and a machine that stays
// silent looks gone. Returns the reply length, or -1 when the packet is not a ping request.
int buildPingReply(uint8_t *packet, int length);

// point payload at the UDP contents of a received packet. returns the payload length, or -1 when
// the packet is not IPv4 UDP, is malformed, or is not addressed to the expected port.
//
// length may include the padding WireGuard adds; the packet's own length field is what counts.
int readUdpPacket(const uint8_t *packet, int length, uint16_t expectedPort, uint32_t *sourceAddress,
                  const uint8_t **payload);
