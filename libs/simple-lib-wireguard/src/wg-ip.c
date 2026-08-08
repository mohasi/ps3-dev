#include "wg-ip.h"

#include "string-utilities.h"
#include "wg-bytes.h"

#define IP_PROTOCOL_UDP    17
#define IP_PROTOCOL_ICMP   1
#define IP_VERSION_IHL     0x45   // version 4, header of five 32 bit words
#define DEFAULT_TTL        64
#define ICMP_HEADER_LENGTH 8
#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8

// add up the 16 bit words, fold the carries back in, invert
uint16_t getInternetChecksum(const uint8_t *data, int length)
{
   uint32_t sum = 0;
   for (int index = 0; index + 1 < length; index += 2) sum += load16be(data + index);
   if (length & 1) sum += (uint32_t)data[length - 1] << 8;

   while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
   return (uint16_t)~sum;
}

// UDP's checksum covers a few IP fields as well as its own, so they are summed in separately
static uint16_t getUdpChecksum(uint32_t sourceAddress, uint32_t destinationAddress, const uint8_t *udp, int udpLength)
{
   uint32_t sum = (sourceAddress >> 16) + (sourceAddress & 0xFFFF) + (destinationAddress >> 16) +
                  (destinationAddress & 0xFFFF) + IP_PROTOCOL_UDP + (uint32_t)udpLength;

   for (int index = 0; index + 1 < udpLength; index += 2) sum += load16be(udp + index);
   if (udpLength & 1) sum += (uint32_t)udp[udpLength - 1] << 8;

   while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);

   // an all-zero checksum means "not computed", so the standard sends the other representation
   uint16_t checksum = (uint16_t)~sum;
   return checksum == 0 ? 0xFFFF : checksum;
}

int buildUdpPacket(uint8_t *packet, int capacity, uint32_t sourceAddress, uint32_t destinationAddress,
                   uint16_t sourcePort, uint16_t destinationPort, const uint8_t *payload, int payloadLength)
{
   int udpLength = UDP_HEADER_LENGTH + payloadLength;
   int totalLength = IPV4_HEADER_LENGTH + udpLength;
   if (capacity < totalLength || payloadLength < 0) return -1;

   memSet(packet, 0, IPV4_HEADER_LENGTH);
   packet[0] = IP_VERSION_IHL;
   store16be(packet + 2, (uint16_t)totalLength);
   packet[8] = DEFAULT_TTL;
   packet[9] = IP_PROTOCOL_UDP;
   store32be(packet + 12, sourceAddress);
   store32be(packet + 16, destinationAddress);
   store16be(packet + 10, getInternetChecksum(packet, IPV4_HEADER_LENGTH));

   uint8_t *udp = packet + IPV4_HEADER_LENGTH;
   store16be(udp, sourcePort);
   store16be(udp + 2, destinationPort);
   store16be(udp + 4, (uint16_t)udpLength);
   store16be(udp + 6, 0);   // zero while the rest is summed, then replaced
   memCopy(udp + UDP_HEADER_LENGTH, payload, payloadLength);
   store16be(udp + 6, getUdpChecksum(sourceAddress, destinationAddress, udp, udpLength));

   return totalLength;
}

int buildPingRequest(uint8_t *packet, int capacity, uint32_t sourceAddress, uint32_t destinationAddress,
                     uint16_t identifier, uint16_t sequence, int payloadLength)
{
   int icmpLength = ICMP_HEADER_LENGTH + payloadLength;
   int totalLength = IPV4_HEADER_LENGTH + icmpLength;
   if (capacity < totalLength || payloadLength < 0) return -1;

   memSet(packet, 0, IPV4_HEADER_LENGTH);
   packet[0] = IP_VERSION_IHL;
   store16be(packet + 2, (uint16_t)totalLength);
   packet[8] = DEFAULT_TTL;
   packet[9] = IP_PROTOCOL_ICMP;
   store32be(packet + 12, sourceAddress);
   store32be(packet + 16, destinationAddress);
   store16be(packet + 10, getInternetChecksum(packet, IPV4_HEADER_LENGTH));

   uint8_t *icmp = packet + IPV4_HEADER_LENGTH;
   memSet(icmp, 0, ICMP_HEADER_LENGTH);
   icmp[0] = ICMP_ECHO_REQUEST;
   store16be(icmp + 4, identifier);
   store16be(icmp + 6, sequence);

   // the filler only has to be something, and a counting pattern makes a corrupted reply obvious
   for (int index = 0; index < payloadLength; index++) icmp[ICMP_HEADER_LENGTH + index] = (uint8_t)index;
   store16be(icmp + 2, getInternetChecksum(icmp, icmpLength));

   return totalLength;
}

int isPingReplyFor(const uint8_t *packet, int length, uint16_t identifier, uint16_t sequence)
{
   if (length < IPV4_HEADER_LENGTH + ICMP_HEADER_LENGTH) return 0;
   if ((packet[0] >> 4) != 4 || packet[9] != IP_PROTOCOL_ICMP) return 0;

   int headerLength = (packet[0] & 0x0F) * 4;
   if (length < headerLength + ICMP_HEADER_LENGTH) return 0;

   const uint8_t *icmp = packet + headerLength;
   return icmp[0] == ICMP_ECHO_REPLY && load16be(icmp + 4) == identifier && load16be(icmp + 6) == sequence;
}

int buildPingReply(uint8_t *packet, int length)
{
   if (length < IPV4_HEADER_LENGTH + ICMP_HEADER_LENGTH) return -1;
   if ((packet[0] >> 4) != 4 || packet[9] != IP_PROTOCOL_ICMP) return -1;

   int headerLength = (packet[0] & 0x0F) * 4;
   int totalLength = load16be(packet + 2);
   if (totalLength > length || totalLength < headerLength + ICMP_HEADER_LENGTH) return -1;

   uint8_t *icmp = packet + headerLength;
   if (icmp[0] != ICMP_ECHO_REQUEST) return -1;

   // send it back where it came from
   uint32_t sender = load32be(packet + 12);
   store32be(packet + 12, load32be(packet + 16));
   store32be(packet + 16, sender);
   store16be(packet + 10, 0);
   store16be(packet + 10, getInternetChecksum(packet, headerLength));

   // the identifier, sequence number and payload all go back unchanged; only the type changes
   icmp[0] = ICMP_ECHO_REPLY;
   store16be(icmp + 2, 0);
   store16be(icmp + 2, getInternetChecksum(icmp, totalLength - headerLength));

   return totalLength;
}

int readUdpPacket(const uint8_t *packet, int length, uint16_t expectedPort, uint32_t *sourceAddress,
                  const uint8_t **payload)
{
   if (length < IPV4_HEADER_LENGTH) return -1;
   if ((packet[0] >> 4) != 4 || packet[9] != IP_PROTOCOL_UDP) return -1;

   int headerLength = (packet[0] & 0x0F) * 4;
   int totalLength = load16be(packet + 2);
   if (headerLength < IPV4_HEADER_LENGTH || totalLength < headerLength + UDP_HEADER_LENGTH) return -1;
   if (totalLength > length) return -1;   // the packet claims more than arrived

   const uint8_t *udp = packet + headerLength;
   if (load16be(udp + 2) != expectedPort) return -1;

   int udpLength = load16be(udp + 4);
   if (udpLength < UDP_HEADER_LENGTH || headerLength + udpLength > totalLength) return -1;

   if (sourceAddress) *sourceAddress = load32be(packet + 12);
   *payload = udp + UDP_HEADER_LENGTH;
   return udpLength - UDP_HEADER_LENGTH;
}
