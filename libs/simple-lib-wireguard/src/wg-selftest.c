#include "wg-selftest.h"

#include "blake2s.h"
#include "chacha20-poly1305.h"
#include "chacha20.h"
#include "dbg.h"
#include "poly1305.h"
#include "string-utilities.h"
#include "syscall.h"
#include "wg-bytes.h"
#include "wg-config.h"
#include "wg-dns.h"
#include "wg-ip.h"
#include "wg-random.h"
#include "wg-replay.h"
#include "wg-tcp.h"
#include "x25519.h"

#define TAG "[wg] "

// Every expected value below is copied from the published document named beside it. Nothing here
// is derived from our own output, which is the whole point: the vectors are what makes the code
// verifiable rather than merely self-consistent.

// RFC 8439 section 2.3.2
static const uint8_t chachaBlockKey[32] = {
   0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
   0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};
static const uint8_t chachaBlockNonce[12] = { 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4A, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t chachaBlockExpected[64] = {
   0x10, 0xF1, 0xE7, 0xE4, 0xD1, 0x3B, 0x59, 0x15, 0x50, 0x0F, 0xDD, 0x1F, 0xA3, 0x20, 0x71, 0xC4,
   0xC7, 0xD1, 0xF4, 0xC7, 0x33, 0xC0, 0x68, 0x03, 0x04, 0x22, 0xAA, 0x9A, 0xC3, 0xD4, 0x6C, 0x4E,
   0xD2, 0x82, 0x64, 0x46, 0x07, 0x9F, 0xAA, 0x09, 0x14, 0xC2, 0xD7, 0x05, 0xD9, 0x8B, 0x02, 0xA2,
   0xB5, 0x12, 0x9C, 0xD1, 0xDE, 0x16, 0x4E, 0xB9, 0xCB, 0xD0, 0x83, 0xE8, 0xA2, 0x50, 0x3C, 0x4E
};

// RFC 8439 section 2.5.2
static const uint8_t polyKey[32] = {
   0x85, 0xD6, 0xBE, 0x78, 0x57, 0x55, 0x6D, 0x33, 0x7F, 0x44, 0x52, 0xFE, 0x42, 0xD5, 0x06, 0xA8,
   0x01, 0x03, 0x80, 0x8A, 0xFB, 0x0D, 0xB2, 0xFD, 0x4A, 0xBF, 0xF6, 0xAF, 0x41, 0x49, 0xF5, 0x1B
};
static const char polyMessage[] = "Cryptographic Forum Research Group";
static const uint8_t polyExpected[16] = {
   0xA8, 0x06, 0x1D, 0xC1, 0x30, 0x51, 0x36, 0xC6, 0xC2, 0x2B, 0x8B, 0xAF, 0x0C, 0x01, 0x27, 0xA9
};

// RFC 8439 section 2.8.2
static const uint8_t aeadKey[32] = {
   0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
   0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F
};
static const uint8_t aeadNonce[12] = { 0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
static const uint8_t aeadAad[12] = { 0x50, 0x51, 0x52, 0x53, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7 };
static const char aeadPlain[] =
   "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
static const uint8_t aeadCipherHead[16] = {
   0xD3, 0x1A, 0x8D, 0x34, 0x64, 0x8E, 0x60, 0xDB, 0x7B, 0x86, 0xAF, 0xBC, 0x53, 0xEF, 0x7E, 0xC2
};
static const uint8_t aeadExpectedTag[16] = {
   0x1A, 0xE1, 0x0B, 0x59, 0x4F, 0x09, 0xE2, 0x6A, 0x7E, 0x90, 0x2E, 0xCB, 0xD0, 0x60, 0x06, 0x91
};

// RFC 7693 appendix B
static const uint8_t blakeExpected[32] = {
   0x50, 0x8C, 0x5E, 0x8C, 0x32, 0x7C, 0x14, 0xE2, 0xE1, 0xA7, 0x2B, 0xA3, 0x4E, 0xEB, 0x45, 0x2F,
   0x37, 0x45, 0x8B, 0x20, 0x9E, 0xD6, 0x3A, 0x29, 0x4D, 0x99, 0x9B, 0x4C, 0x86, 0x67, 0x59, 0x82
};

// BLAKE2 reference test vectors, testvectors/blake2s-kat.txt, first three keyed entries.
// key is bytes 0x00..0x1f; the messages are empty, one byte, and two bytes.
static const uint8_t keyedBlakeKey[32] = {
   0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
   0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};
static const uint8_t keyedBlakeExpected[3][32] = {
   { 0x48, 0xA8, 0x99, 0x7D, 0xA4, 0x07, 0x87, 0x6B, 0x3D, 0x79, 0xC0, 0xD9, 0x23, 0x25, 0xAD, 0x3B,
     0x89, 0xCB, 0xB7, 0x54, 0xD8, 0x6A, 0xB7, 0x1A, 0xEE, 0x04, 0x7A, 0xD3, 0x45, 0xFD, 0x2C, 0x49 },
   { 0x40, 0xD1, 0x5F, 0xEE, 0x7C, 0x32, 0x88, 0x30, 0x16, 0x6A, 0xC3, 0xF9, 0x18, 0x65, 0x0F, 0x80,
     0x7E, 0x7E, 0x01, 0xE1, 0x77, 0x25, 0x8C, 0xDC, 0x0A, 0x39, 0xB1, 0x1F, 0x59, 0x80, 0x66, 0xF1 },
   { 0x6B, 0xB7, 0x13, 0x00, 0x64, 0x4C, 0xD3, 0x99, 0x1B, 0x26, 0xCC, 0xD4, 0xD2, 0x74, 0xAC, 0xD1,
     0xAD, 0xEA, 0xB8, 0xB1, 0xD7, 0x91, 0x45, 0x46, 0xC1, 0x19, 0x8B, 0xBE, 0x9F, 0xC9, 0xD8, 0x03 }
};

// Linux kernel lib/crypto/blake2s-selftest.c (v5.15), blake2s_hmac_testvecs. the key is a
// Fibonacci sequence of 32 bytes and the message is the 256 bytes 0x00..0xff; the second entry
// swaps the two, which exercises the path where the key is longer than a block.
static const uint8_t hmacExpected[2][32] = {
   { 0xCE, 0xE1, 0x57, 0x69, 0x82, 0xDC, 0xBF, 0x43, 0xAD, 0x56, 0x4C, 0x70, 0xED, 0x68, 0x16, 0x96,
     0xCF, 0xA4, 0x73, 0xE8, 0xE8, 0xFC, 0x32, 0x79, 0x08, 0x0A, 0x75, 0x82, 0xDA, 0x3F, 0x05, 0x11 },
   { 0x77, 0x2F, 0x0C, 0x71, 0x41, 0xF4, 0x4B, 0x2B, 0xB3, 0xC6, 0xB6, 0xF9, 0x60, 0xDE, 0xE4, 0x52,
     0x38, 0x66, 0xE8, 0xBF, 0x9B, 0x96, 0xC4, 0x9F, 0x60, 0xD9, 0x24, 0x37, 0x99, 0xD6, 0xEC, 0x31 }
};

// a config in the shape providers hand out. the keys are base64 of the bytes 0x00..0x1f and
// 0x20..0x3f, so the decoded values are checkable; no real key is ever compiled in.
static const char sampleConfig[] =
   "[Interface]\n"
   "# a comment line\n"
   "PrivateKey = AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\n"
   "Address = 10.2.0.2/32\n"
   "DNS = 10.2.0.1\n"
   "MTU = 1420\n"
   "\n"
   "[Peer]\n"
   "PublicKey = ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=\n"
   "AllowedIPs = 0.0.0.0/0, ::/0\n"
   "Endpoint = 203.0.113.7:51820\n"
   "PersistentKeepalive = 25\n";

// RFC 7748 section 6.1
static const uint8_t alicePrivate[32] = {
   0x77, 0x07, 0x6D, 0x0A, 0x73, 0x18, 0xA5, 0x7D, 0x3C, 0x16, 0xC1, 0x72, 0x51, 0xB2, 0x66, 0x45,
   0xDF, 0x4C, 0x2F, 0x87, 0xEB, 0xC0, 0x99, 0x2A, 0xB1, 0x77, 0xFB, 0xA5, 0x1D, 0xB9, 0x2C, 0x2A
};
static const uint8_t alicePublic[32] = {
   0x85, 0x20, 0xF0, 0x09, 0x89, 0x30, 0xA7, 0x54, 0x74, 0x8B, 0x7D, 0xDC, 0xB4, 0x3E, 0xF7, 0x5A,
   0x0D, 0xBF, 0x3A, 0x0D, 0x26, 0x38, 0x1A, 0xF4, 0xEB, 0xA4, 0xA9, 0x8E, 0xAA, 0x9B, 0x4E, 0x6A
};
static const uint8_t bobPrivate[32] = {
   0x5D, 0xAB, 0x08, 0x7E, 0x62, 0x4A, 0x8A, 0x4B, 0x79, 0xE1, 0x7F, 0x8B, 0x83, 0x80, 0x0E, 0xE6,
   0x6F, 0x3B, 0xB1, 0x29, 0x26, 0x18, 0xB6, 0xFD, 0x1C, 0x2F, 0x8B, 0x27, 0xFF, 0x88, 0xE0, 0xEB
};
static const uint8_t bobPublic[32] = {
   0xDE, 0x9E, 0xDB, 0x7D, 0x7B, 0x7D, 0xC1, 0xB4, 0xD3, 0x5B, 0x61, 0xC2, 0xEC, 0xE4, 0x35, 0x37,
   0x3F, 0x83, 0x43, 0xC8, 0x5B, 0x78, 0x67, 0x4D, 0xAD, 0xFC, 0x7E, 0x14, 0x6F, 0x88, 0x2B, 0x4F
};
static const uint8_t sharedExpected[32] = {
   0x4A, 0x5D, 0x9D, 0x5B, 0xA4, 0xCE, 0x2D, 0xE1, 0x72, 0x8E, 0x3B, 0xF4, 0x80, 0x35, 0x0F, 0x25,
   0xE0, 0x7E, 0x21, 0xC9, 0x47, 0xD1, 0x9E, 0x33, 0x76, 0xF0, 0x9B, 0x3C, 0x1E, 0x16, 0x17, 0x42
};

static int checkVector(const char *name, const uint8_t *actual, const uint8_t *expected, int length)
{
   if (bytesEqual(actual, expected, length)) {
      logTrace(TAG "%s ok\n", name);
      return 0;
   }

   // show the first mismatching byte: it says whether the result is completely wrong or off by a
   // carry somewhere late in the buffer, which points at very different bugs.
   int position = 0;
   while (position < length && actual[position] == expected[position]) position++;
   logError(TAG "%s FAILED at byte %d, got 0x%02x expected 0x%02x\n", name, position, actual[position],
            expected[position]);
   return 1;
}

// the replay window has no published vector, so these are the rules from the protocol written out
// as cases: a repeat is refused, out of order still gets through, and anything older than the
// window is refused because it cannot be judged.
static int checkReplayWindow(void)
{
   WgReplayWindow window;
   resetWgReplayWindow(&window);

   const char *wrongCase = 0;
   if (!acceptWgCounter(&window, 0)) wrongCase = "first packet";
   else if (acceptWgCounter(&window, 0)) wrongCase = "repeat of the first packet";
   else if (!acceptWgCounter(&window, 1) || !acceptWgCounter(&window, 2)) wrongCase = "packets in order";
   else if (acceptWgCounter(&window, 1)) wrongCase = "repeat of an earlier packet";
   else if (!acceptWgCounter(&window, 100)) wrongCase = "a jump forward";
   else if (!acceptWgCounter(&window, 50)) wrongCase = "arriving out of order";
   else if (acceptWgCounter(&window, 50)) wrongCase = "repeat of an out of order packet";
   else if (acceptWgCounter(&window, 30)) wrongCase = "a packet older than the window";
   else if (!acceptWgCounter(&window, 101)) wrongCase = "carrying on after the window moved";

   if (wrongCase) {
      logError(TAG "replay window FAILED on %s\n", wrongCase);
      return 1;
   }

   logTrace(TAG "replay window ok\n");
   return 0;
}

// A reply built by hand from the format in RFC 1035, so the expected answer is known: one
// question for "a.example", one address record holding 93.184.216.34, and a name in the answer
// that points back at the question rather than repeating it.
static const uint8_t dnsReply[] = {
   0x12, 0x34,                                       // the id we will ask about
   0x81, 0x80,                                       // a reply, no error
   0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,   // one question, one answer
   0x01, 'a', 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x00,
   0x00, 0x01, 0x00, 0x01,                           // asking for an address
   0xC0, 0x0C,                                       // the name, pointing back at the question
   0x00, 0x01, 0x00, 0x01,                           // an address record
   0x00, 0x00, 0x0E, 0x10,                           // how long it may be cached
   0x00, 0x04, 93, 184, 216, 34                      // four bytes: the address
};

static int checkIpAndDns(void)
{
   const char *wrongCase = 0;

   // a UDP packet we build should be one a receiver accepts: summing a correct header gives zero
   uint8_t packet[128];
   const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01 };
   int packetLength = buildUdpPacket(packet, sizeof packet, 0x0A020002, 0x0A020001, 5353, DNS_PORT, payload,
                                     sizeof payload);
   if (packetLength != IPV4_HEADER_LENGTH + UDP_HEADER_LENGTH + (int)sizeof payload) wrongCase = "udp packet length";
   else if (getInternetChecksum(packet, IPV4_HEADER_LENGTH) != 0) wrongCase = "ip header checksum";

   // and one we read back should give the payload unchanged, even with padding on the end
   if (!wrongCase) {
      const uint8_t *readBack = 0;
      uint32_t source = 0;
      int readLength = readUdpPacket(packet, sizeof packet, DNS_PORT, &source, &readBack);
      if (readLength != (int)sizeof payload) wrongCase = "udp payload length";
      else if (source != 0x0A020002) wrongCase = "udp source address";
      else if (!bytesEqual(readBack, payload, sizeof payload)) wrongCase = "udp payload contents";
      else if (readUdpPacket(packet, sizeof packet, 1234, 0, &readBack) >= 0) wrongCase = "udp wrong port accepted";
   }

   // the query we build has to start with the id and carry the name as labels
   if (!wrongCase) {
      uint8_t query[64];
      int queryLength = buildDnsQuery(query, sizeof query, 0x1234, "a.example");
      static const uint8_t expectedQuestion[] = { 0x01, 'a', 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x00,
                                                 0x00, 0x01, 0x00, 0x01 };
      if (queryLength != 12 + (int)sizeof expectedQuestion) wrongCase = "dns query length";
      else if (!bytesEqual(query + 12, expectedQuestion, sizeof expectedQuestion)) wrongCase = "dns query question";
   }

   // a ping request the gateway might send, and the reply we must send back
   if (!wrongCase) {
      uint8_t ping[IPV4_HEADER_LENGTH + 12];
      memSet(ping, 0, sizeof ping);
      ping[0] = 0x45;
      ping[2] = 0;
      ping[3] = (uint8_t)sizeof ping;
      ping[8] = 64;
      ping[9] = 1;                                        // icmp
      ping[12] = 10; ping[13] = 2; ping[14] = 0; ping[15] = 1;   // from 10.2.0.1
      ping[16] = 10; ping[17] = 2; ping[18] = 0; ping[19] = 2;   // to 10.2.0.2
      ping[20] = 8;                                       // echo request
      ping[24] = 0xAB; ping[25] = 0xCD;                   // identifier, echoed back unchanged

      int replyLength = buildPingReply(ping, sizeof ping);
      if (replyLength != (int)sizeof ping) wrongCase = "ping reply length";
      else if (ping[20] != 0) wrongCase = "ping reply type";
      else if (ping[12] != 10 || ping[15] != 2) wrongCase = "ping reply not sent back to the sender";
      else if (ping[24] != 0xAB || ping[25] != 0xCD) wrongCase = "ping reply identifier";
      else if (getInternetChecksum(ping, IPV4_HEADER_LENGTH) != 0) wrongCase = "ping reply ip checksum";
      else if (getInternetChecksum(ping + IPV4_HEADER_LENGTH, replyLength - IPV4_HEADER_LENGTH) != 0)
         wrongCase = "ping reply icmp checksum";
      else if (buildPingReply(ping, replyLength) >= 0) wrongCase = "a reply was treated as a request";
   }

   if (!wrongCase) {
      uint32_t address = 0;
      if (readDnsAnswer(dnsReply, sizeof dnsReply, 0x1234, &address) != 0) wrongCase = "dns answer";
      else if (address != 0x5DB8D822) wrongCase = "dns address";                       // 93.184.216.34
      else if (readDnsAnswer(dnsReply, sizeof dnsReply, 0x9999, &address) == 0) wrongCase = "dns wrong id accepted";
   }

   if (wrongCase) {
      logError(TAG "ip and dns FAILED on %s\n", wrongCase);
      return 1;
   }

   logTrace(TAG "ip and dns ok\n");
   return 0;
}

// the config check reports the first field that is wrong rather than one line per field, so a
// pass stays one line and a failure names exactly what to look at.
static int checkConfig(void)
{
   WgConfig config;
   if (parseWgConfig(&config, sampleConfig, (int)(sizeof sampleConfig - 1)) != 0) {
      logError(TAG "config parse FAILED\n");
      return 1;
   }

   uint8_t expectedPublicKey[32];
   for (int index = 0; index < 32; index++) expectedPublicKey[index] = (uint8_t)(0x20 + index);

   const char *wrongField = 0;
   if (!bytesEqual(config.privateKey, keyedBlakeKey, 32)) wrongField = "PrivateKey";
   else if (!bytesEqual(config.peerPublicKey, expectedPublicKey, 32)) wrongField = "PublicKey";
   else if (config.tunnelAddress != 0x0A020002u) wrongField = "Address";
   else if (config.tunnelPrefixLength != 32) wrongField = "Address prefix";
   else if (config.dnsAddress != 0x0A020001u) wrongField = "DNS";
   else if (!strEq(config.endpointHost, "203.0.113.7")) wrongField = "Endpoint host";
   else if (config.endpointPort != 51820) wrongField = "Endpoint port";
   else if (config.keepaliveSeconds != 25) wrongField = "PersistentKeepalive";
   else if (!config.routesAllTraffic) wrongField = "AllowedIPs";
   else if (config.hasPresharedKey) wrongField = "PresharedKey";

   if (wrongField) {
      logError(TAG "config %s FAILED\n", wrongField);
      return 1;
   }

   logTrace(TAG "config parse ok\n");
   return 0;
}

// The stream layer, against the two things an ordinary download never produces: a server that sends
// the last of a page and closes in the same packet, and a peer that sends a byte into a full ring
// to ask whether it has emptied. Both were handled wrongly until they were checked here.
static int checkTcpStream(void)
{
   static WgTcpConnection connection;   // the rings it points at are borrowed for the test and given back
   const char *wrongCase = 0;

   uint32_t buffers = 0;
   if (sysMemAllocate(WG_TCP_BUFFER_MAX, SYS_PAGE_64K, &buffers) != 0 || buffers == 0) {
      logError(TAG "tcp stream FAILED, no room for a stream's buffers\n");
      return 1;
   }

   connection.outgoing = (uint8_t *)(uintptr_t)buffers;
   connection.incoming = connection.outgoing + WG_TCP_SEND_MAX;
   openTcpConnection(&connection, 0x0A020001u, 80, 50000, 1000);

   // the answer to our hello, offering scaling and a full sized segment
   TcpSegment segment;
   memSet(&segment, 0, sizeof segment);
   segment.flags = TCP_FLAG_SYN | TCP_FLAG_ACK;
   segment.sequence = 5000;
   segment.acknowledgment = 1001;
   segment.window = 512;
   segment.segmentMax = 1400;
   segment.hasWindowScale = 1;
   segment.windowShift = 7;
   if (processTcpSegment(&connection, &segment, 0) != TCP_REACT_ACKNOWLEDGE || connection.state != WG_TCP_OPEN)
      wrongCase = "the answer to a hello";

   // five bytes and a goodbye in one segment
   memSet(&segment, 0, sizeof segment);
   segment.flags = TCP_FLAG_ACK | TCP_FLAG_PSH | TCP_FLAG_FIN;
   segment.sequence = 5001;
   segment.acknowledgment = 1001;
   segment.window = 512;
   segment.data = (const uint8_t *)"hello";
   segment.dataLength = 5;
   processTcpSegment(&connection, &segment, 0);

   char received[8];
   int readable = getTcpReadable(&connection);
   int taken = readTcpIncoming(&connection, (uint8_t *)received, sizeof received - 1);
   received[taken > 0 ? taken : 0] = 0;

   if (!wrongCase && !connection.remoteHasFinished) wrongCase = "a goodbye carrying data";
   else if (!wrongCase && readable != 5) wrongCase = "the goodbye counted as a byte to read";
   else if (!wrongCase && !strEq(received, "hello")) wrongCase = "the data a goodbye carried";

   // a byte arriving with no room left for it
   connection.remoteHasFinished = 0;
   connection.receiveStart = 6000;
   connection.receiveNext = 6000 + WG_TCP_RECEIVE_MAX;
   segment.flags = TCP_FLAG_ACK;
   segment.sequence = connection.receiveNext;
   segment.data = (const uint8_t *)"x";
   segment.dataLength = 1;
   processTcpSegment(&connection, &segment, 0);
   if (!wrongCase && connection.receiveNext != 6000 + WG_TCP_RECEIVE_MAX) wrongCase = "a byte taken with no room for it";

   sysMemFree(buffers);

   if (wrongCase) {
      logError(TAG "tcp stream FAILED on %s\n", wrongCase);
      return 1;
   }

   logTrace(TAG "tcp stream ok\n");
   return 0;
}

int runWgSelfTest(void)
{
   int failures = 0;

   // chacha20 keystream block
   uint8_t block[64];
   getChaCha20Block(block, chachaBlockKey, chachaBlockNonce, 1);
   failures += checkVector("chacha20 block", block, chachaBlockExpected, sizeof chachaBlockExpected);

   // poly1305 tag
   uint8_t tag[16];
   authPoly1305(tag, polyKey, polyMessage, (int)(sizeof polyMessage - 1));
   failures += checkVector("poly1305 tag", tag, polyExpected, sizeof polyExpected);

   // aead seal: the tag covers the whole ciphertext, so matching it validates the cipher, the
   // authenticator and the padding rules in one check
   int plainLength = (int)(sizeof aeadPlain - 1);
   uint8_t sealed[sizeof aeadPlain - 1 + AEAD_TAG_LENGTH];
   sealChaCha20Poly1305(sealed, (const uint8_t *)aeadPlain, plainLength, aeadAad, sizeof aeadAad, aeadNonce, aeadKey);
   failures += checkVector("aead ciphertext head", sealed, aeadCipherHead, sizeof aeadCipherHead);
   failures += checkVector("aead tag", sealed + plainLength, aeadExpectedTag, sizeof aeadExpectedTag);

   // aead open: accepts a good message and rejects a flipped bit
   uint8_t opened[sizeof aeadPlain - 1];
   int accepted = openChaCha20Poly1305(opened, sealed, plainLength + AEAD_TAG_LENGTH, aeadAad, sizeof aeadAad,
                                       aeadNonce, aeadKey);
   if (accepted && bytesEqual(opened, (const uint8_t *)aeadPlain, plainLength)) {
      logTrace(TAG "aead open ok\n");
   } else {
      logError(TAG "aead open FAILED, accepted=%d\n", accepted);
      failures++;
   }

   sealed[0] ^= 0x01;
   if (openChaCha20Poly1305(opened, sealed, plainLength + AEAD_TAG_LENGTH, aeadAad, sizeof aeadAad, aeadNonce,
                            aeadKey)) {
      logError(TAG "aead tamper FAILED, a corrupted message was accepted\n");
      failures++;
   } else {
      logTrace(TAG "aead tamper rejected ok\n");
   }

   // blake2s-256 of "abc"
   uint8_t hash[32];
   hashBlake2s(hash, sizeof hash, "abc", 3);
   failures += checkVector("blake2s abc", hash, blakeExpected, sizeof blakeExpected);

   // keyed blake2s over messages of 0, 1 and 2 bytes
   static const char *keyedNames[3] = { "blake2s keyed 0", "blake2s keyed 1", "blake2s keyed 2" };
   const uint8_t keyedMessage[2] = { 0x00, 0x01 };
   for (int messageLength = 0; messageLength < 3; messageLength++) {
      macBlake2s(hash, sizeof hash, keyedBlakeKey, sizeof keyedBlakeKey, keyedMessage, messageLength);
      failures += checkVector(keyedNames[messageLength], hash, keyedBlakeExpected[messageLength], sizeof hash);
   }

   // hmac-blake2s, once with a short key and once with a key longer than a block
   uint8_t hmacKey[32];
   hmacKey[0] = 1;
   hmacKey[1] = 1;
   for (int index = 2; index < (int)sizeof hmacKey; index++)
      hmacKey[index] = (uint8_t)(hmacKey[index - 2] + hmacKey[index - 1]);

   uint8_t hmacMessage[256];
   for (int index = 0; index < (int)sizeof hmacMessage; index++) hmacMessage[index] = (uint8_t)index;

   hmacBlake2s(hash, hmacKey, sizeof hmacKey, hmacMessage, sizeof hmacMessage);
   failures += checkVector("hmac-blake2s", hash, hmacExpected[0], sizeof hash);

   hmacBlake2s(hash, hmacMessage, sizeof hmacMessage, hmacKey, sizeof hmacKey);
   failures += checkVector("hmac-blake2s long key", hash, hmacExpected[1], sizeof hash);

   // config file parsing, including the base64 key decoding
   failures += checkConfig();

   failures += checkReplayWindow();
   failures += checkIpAndDns();
   failures += checkTcpStream();

   // the random source. this only proves the console answers and does not repeat itself; the
   // quality of the randomness cannot be tested from here and rests on the source it comes from.
   uint8_t firstDraw[32], secondDraw[32];
   if (getRandomBytes(firstDraw, sizeof firstDraw) != 0 || getRandomBytes(secondDraw, sizeof secondDraw) != 0) {
      failures++;   // getRandomBytes already logged why
   } else if (bytesEqual(firstDraw, secondDraw, sizeof firstDraw)) {
      logError(TAG "random source FAILED, two draws were identical\n");
      failures++;
   } else {
      logTrace(TAG "random source ok\n");
   }

   // x25519 public keys and both sides of the exchange
   uint8_t publicKey[32], shared[32];
   getX25519PublicKey(publicKey, alicePrivate);
   failures += checkVector("x25519 alice public", publicKey, alicePublic, sizeof alicePublic);

   getX25519PublicKey(publicKey, bobPrivate);
   failures += checkVector("x25519 bob public", publicKey, bobPublic, sizeof bobPublic);

   computeX25519Shared(shared, alicePrivate, bobPublic);
   failures += checkVector("x25519 shared from alice", shared, sharedExpected, sizeof sharedExpected);

   computeX25519Shared(shared, bobPrivate, alicePublic);
   failures += checkVector("x25519 shared from bob", shared, sharedExpected, sizeof sharedExpected);

   if (failures == 0) logInfo(TAG "self test passed\n");
   else logError(TAG "self test FAILED, %d case(s)\n", failures);
   return failures;
}
