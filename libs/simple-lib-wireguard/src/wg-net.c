#include "wg-net.h"

#include <sys/sys_time.h>

#include "dbg.h"
#include "syscall.h"   // the stream buffers are taken from the kernel rather than sitting in BSS
#include "string-utilities.h"
#include "wg-bytes.h"
#include "wg-dns.h"
#include "wg-ip.h"
#include "wg-random.h"
#include "wg-tcp.h"

#define TAG "[wg] "

#define FIRST_CHOSEN_PORT 49152   // where ports we pick for the caller start
#define CHOSEN_PORT_COUNT  8192   // how many of them there are to pick from
#define PROBE_IDENTIFIER  0x5057  // marks our own pings, echoed back unchanged
#define QUEUE_SLOT_COUNT  64      // packets held across the whole app while it is between calls
#define RESOLVE_STEP_MS   200
#define RESOLVE_RESEND_MS 1000   // how often the question goes again while no answer has come back

typedef struct {
   uint8_t    packet[WG_PACKET_MAX];
   int        length;
   WgEndpoint from;
   int        socketIndex;   // -1 when the slot is free
} QueuedPacket;

typedef struct {
   int      isOpen;
   uint16_t localPort;
} UdpSocket;

// one VPN per app, so the state is here rather than in a handle the caller has to carry
static WgTunnel tunnel;
static UdpSocket sockets[WG_SOCKET_MAX];
static QueuedPacket queue[QUEUE_SLOT_COUNT];
static WgTcpConnection streams[WG_TCP_MAX];
static int streamInUse[WG_TCP_MAX];
static int reportedAllInUse;   // so the "all in use" line is said once rather than on every attempt
static int isStarted;
static uint64_t droppedPackets;

static uint64_t getMillisecondsNow(void)
{
   return sys_time_get_system_time() / 1000;
}

// Both rings of one stream, in a single allocation the kernel hands out in 64 KB pages. 384 KB is
// six of them exactly. Holding sixteen of these in fixed memory would cost six megabytes whether or
// not anything is connected, which is what an app with many short lived peers must not pay.
static int takeStreamBuffers(WgTcpConnection *stream)
{
   uint32_t address = 0;
   int32_t result = sysMemAllocate(WG_TCP_BUFFER_MAX, SYS_PAGE_64K, &address);
   if (result != 0 || address == 0) {
      logError(TAG "tcp: no room for a stream's %d KB of buffer, rc=0x%x\n", WG_TCP_BUFFER_MAX / 1024, result);
      return -1;
   }

   stream->outgoing = (uint8_t *)(uintptr_t)address;
   stream->incoming = stream->outgoing + WG_TCP_SEND_MAX;
   return 0;
}

static void giveBackStreamBuffers(WgTcpConnection *stream)
{
   if (!stream->outgoing) return;

   memSet(stream->outgoing, 0, WG_TCP_BUFFER_MAX);   // they held whatever was being carried
   sysMemFree((uint32_t)(uintptr_t)stream->outgoing);
   stream->outgoing = 0;
   stream->incoming = 0;
}

// A segment that reached the tunnel carried our acknowledgement with it. One that did not leaves
// the count alone, so the next call sends it again rather than the other end waiting for its timer.
static int sendStreamPacket(WgTcpConnection *stream, const uint8_t *packet, int length)
{
   if (sendThroughWgTunnel(&tunnel, packet, length) != 0) return -1;

   stream->segmentsSinceAcknowledgement = 0;
   return 0;
}

// send one segment carrying no data: a hello, an acknowledgement or a goodbye
static int sendStreamControl(WgTcpConnection *stream, uint8_t flags)
{
   uint8_t packet[WG_PACKET_MAX];
   int packetLength = buildTcpControl(packet, sizeof packet, stream, tunnel.config.tunnelAddress, flags);
   if (packetLength < 0) return -1;

   return sendStreamPacket(stream, packet, packetLength);
}

// put out as much waiting data as the other end and the link will take. returns how many segments
// went, which matters because each one carries an acknowledgement of its own.
static int flushStream(WgTcpConnection *stream, uint64_t nowMs)
{
   uint8_t packet[WG_PACKET_MAX];
   int sent = 0;

   for (;;) {
      int packetLength = buildTcpData(packet, sizeof packet, stream, tunnel.config.tunnelAddress, nowMs);
      if (packetLength <= 0) return sent;
      if (sendStreamPacket(stream, packet, packetLength) != 0) return sent;
      sent++;
   }
}

int startWgNetwork(const char *configPath)
{
   memSet(sockets, 0, sizeof sockets);
   memSet(streams, 0, sizeof streams);
   memSet(streamInUse, 0, sizeof streamInUse);
   for (int slot = 0; slot < QUEUE_SLOT_COUNT; slot++) queue[slot].socketIndex = -1;
   droppedPackets = 0;

   WgConfig config;
   if (loadWgConfig(&config, configPath) != 0) return -1;
   if (openWgTunnel(&tunnel, &config) != 0) return -1;

   isStarted = 1;
   return 0;
}

void stopWgNetwork(void)
{
   if (!isStarted) return;

   for (int index = 0; index < WG_TCP_MAX; index++)
      if (streamInUse[index]) giveBackStreamBuffers(&streams[index]);

   closeWgTunnel(&tunnel);
   memSet(sockets, 0, sizeof sockets);
   memSet(streams, 0, sizeof streams);
   memSet(streamInUse, 0, sizeof streamInUse);
   memSet(queue, 0, sizeof queue);   // packets may hold anything, so they do not linger in memory
   isStarted = 0;
}

int isWgNetworkReady(void)
{
   return isStarted && getWgTunnelState(&tunnel) == WG_TUNNEL_UP;
}

const WgTunnel *getWgNetworkTunnel(void)
{
   return &tunnel;
}

// put an arriving packet where the socket that asked for it will find it
static void queuePacket(int socketIndex, const WgEndpoint *from, const uint8_t *payload, int length)
{
   for (int slot = 0; slot < QUEUE_SLOT_COUNT; slot++) {
      if (queue[slot].socketIndex >= 0) continue;

      memCopy(queue[slot].packet, payload, length);
      queue[slot].length = length;
      queue[slot].from = *from;
      queue[slot].socketIndex = socketIndex;
      return;
   }

   // every slot is full: the app is not reading fast enough. UDP is allowed to drop packets, so it
   // does, but the first time is worth saying out loud because it usually means a bug in the app.
   droppedPackets++;
   if (droppedPackets == 1) logWarn(TAG "socket: no room for an arriving packet, the app is not reading them\n");
}

// data that goes unconfirmed for longer than the measured round trip allows is sent again, and a
// stream that never gets through gives up
static void resendLostSegments(void)
{
   uint64_t now = getMillisecondsNow();

   for (int index = 0; index < WG_TCP_MAX; index++) {
      WgTcpConnection *stream = &streams[index];
      if (!streamInUse[index]) continue;
      if (stream->state == WG_TCP_FAILED || stream->state == WG_TCP_CLOSED) continue;
      if (now - stream->oldestSentMs < (uint64_t)stream->resendTimeoutMs) continue;

      // an unanswered hello is often simply lost, so it goes again until they answer or we give up
      if (stream->state == WG_TCP_CONNECTING) {
         if (resendTcpHello(stream, now) == 0) sendStreamControl(stream, TCP_FLAG_SYN);
         continue;
      }

      // a stream that has said goodbye has nothing else outstanding, so the goodbye is what goes
      // out again until they agree to it
      if (stream->state == WG_TCP_CLOSING) {
         stream->oldestSentMs = now;
         sendStreamControl(stream, TCP_FLAG_FIN | TCP_FLAG_ACK);
         continue;
      }

      if (stream->sendNext != stream->sendUnconfirmed) {
         if (resendTcpFromOldest(stream, now) == 0) flushStream(stream, now);
         continue;
      }

      // nothing in flight but data still waiting means they have offered no room and have not said
      // it has opened again. one byte asks, and unlike an acknowledgement it has to be answered.
      if (stream->sendEnd != stream->sendNext) {
         uint8_t packet[WG_PACKET_MAX];
         int packetLength = buildTcpProbe(packet, sizeof packet, stream, tunnel.config.tunnelAddress);

         stream->oldestSentMs = now;
         if (packetLength > 0) sendStreamPacket(stream, packet, packetLength);
      }
   }
}

// hand an arriving packet to the stream it belongs to. returns 1 when it was one.
static int routeToStream(const uint8_t *packet, int length, uint64_t nowMs)
{
   for (int index = 0; index < WG_TCP_MAX; index++) {
      if (!streamInUse[index]) continue;

      TcpSegment segment;
      if (readTcpSegment(packet, length, &streams[index], &segment) != 0) continue;

      TcpReaction reaction = processTcpSegment(&streams[index], &segment, nowMs);

      // a confirmation frees window, so more may go out at once, and anything that does carries the
      // acknowledgement with it
      int sent = flushStream(&streams[index], nowMs);
      if (reaction == TCP_REACT_ACKNOWLEDGE && sent == 0) sendStreamControl(&streams[index], TCP_FLAG_ACK);
      return 1;
   }
   return 0;
}

// give one arriving IP packet to whatever asked for it
static void deliverPacket(const uint8_t *packet, int length, uint64_t nowMs)
{
   if (routeToStream(packet, length, nowMs)) return;

   // work out which socket it belongs to, by the port it was sent to
   const uint8_t *payload = 0;
   uint32_t sourceAddress = 0;
   int payloadLength = -1;
   int socketIndex = -1;

   for (int index = 0; index < WG_SOCKET_MAX && payloadLength < 0; index++) {
      if (!sockets[index].isOpen) continue;

      payloadLength = readUdpPacket(packet, length, sockets[index].localPort, &sourceAddress, &payload);
      if (payloadLength >= 0) socketIndex = index;
   }

   if (socketIndex < 0 || payloadLength > WG_PACKET_MAX) return;   // not ours, or too big to hold

   // the sender's port is the first field of the UDP header, which sits after the IP header
   int headerLength = (packet[0] & 0x0F) * 4;
   WgEndpoint from;
   from.address = sourceAddress;
   from.port = load16be(packet + headerLength);

   queuePacket(socketIndex, &from, payload, payloadLength);
}

// How many datagrams one service call takes before handing back. A stream at full speed delivers
// far more per second than an app's loop turns, so taking one at a time would cap the rate at the
// speed of the loop rather than the link. It also has to outpace what arrives while the app is
// between calls: anything left in the console's socket buffer when it fills up is lost, and a lost
// packet costs a round trip and half the sender's rate. 256 is 350 KB, well past a full buffer.
#define DRAIN_LIMIT 256

int serviceWgNetwork(int waitMs)
{
   if (!isStarted) return -1;

   resendLostSegments();

   // the clock is read once the first packet is in hand, not before: the first read is the one that
   // waits, and emptying the rest of the socket after it takes no measurable time
   uint8_t packet[WG_PACKET_MAX];
   uint64_t now = 0;
   for (int taken = 0; taken < DRAIN_LIMIT; taken++) {
      int arrived = 0;
      int length = serviceWgTunnel(&tunnel, packet, sizeof packet, taken == 0 ? waitMs : 0, &arrived);
      if (length < 0) return -1;
      if (!arrived) break;

      if (taken == 0) now = getMillisecondsNow();
      if (length > 0) deliverPacket(packet, length, now);
   }

   // One acknowledgement covers everything that arrived in the batch. A stream that has room again
   // is told even if nothing arrived for it, because a stopped sender has nothing else to wait for.
   for (int index = 0; index < WG_TCP_MAX; index++)
      if (streamInUse[index] && (streams[index].segmentsSinceAcknowledgement > 0 ||
                                 (streams[index].state == WG_TCP_OPEN && shouldAnnounceWindow(&streams[index]))))
         sendStreamControl(&streams[index], TCP_FLAG_ACK);

   return 0;
}

WgSocket openWgUdp(uint16_t localPort)
{
   if (!isStarted) return -1;

   // two sockets on one port would each get half the packets, so the second is refused
   for (int index = 0; index < WG_SOCKET_MAX; index++) {
      if (localPort != 0 && sockets[index].isOpen && sockets[index].localPort == localPort) {
         logError(TAG "socket: port %d is already open\n", localPort);
         return -1;
      }
   }

   for (int index = 0; index < WG_SOCKET_MAX; index++) {
      if (sockets[index].isOpen) continue;

      sockets[index].isOpen = 1;
      sockets[index].localPort = localPort != 0 ? localPort : (uint16_t)(FIRST_CHOSEN_PORT + index);
      return index;
   }

   logError(TAG "socket: all %d sockets are in use\n", WG_SOCKET_MAX);
   return -1;
}

void closeWgUdp(WgSocket socket)
{
   if (socket < 0 || socket >= WG_SOCKET_MAX) return;

   // drop anything still waiting for it, so a later socket cannot inherit those packets
   for (int slot = 0; slot < QUEUE_SLOT_COUNT; slot++)
      if (queue[slot].socketIndex == socket) queue[slot].socketIndex = -1;

   sockets[socket].isOpen = 0;
}

int sendWgTo(WgSocket socket, const WgEndpoint *to, const void *data, int length)
{
   if (socket < 0 || socket >= WG_SOCKET_MAX || !sockets[socket].isOpen) return -1;

   uint8_t packet[WG_PACKET_MAX];
   int packetLength = buildUdpPacket(packet, sizeof packet, tunnel.config.tunnelAddress, to->address,
                                     sockets[socket].localPort, to->port, (const uint8_t *)data, length);
   if (packetLength < 0) return -1;

   return sendThroughWgTunnel(&tunnel, packet, packetLength);
}

int recvWgFrom(WgSocket socket, WgEndpoint *from, void *buffer, int capacity)
{
   if (socket < 0 || socket >= WG_SOCKET_MAX || !sockets[socket].isOpen) return -1;

   for (int slot = 0; slot < QUEUE_SLOT_COUNT; slot++) {
      if (queue[slot].socketIndex != socket) continue;

      int length = queue[slot].length < capacity ? queue[slot].length : capacity;
      memCopy(buffer, queue[slot].packet, length);
      if (from) *from = queue[slot].from;
      queue[slot].socketIndex = -1;
      return length;
   }

   return 0;
}

#define STREAM_STEP_MS  20   // how long a call that has to wait gives the network before looking again
#define CLOSE_WAIT_MS 1000

static int isLocalPortInUse(uint16_t port)
{
   for (int index = 0; index < WG_TCP_MAX; index++)
      if (streamInUse[index] && streams[index].localPort == port) return 1;

   return 0;
}

WgTcpSocket openWgTcp(uint32_t address, uint16_t port)
{
   if (!isStarted) return -1;

   int index = 0;
   while (index < WG_TCP_MAX && streamInUse[index]) index++;

   // a caller that holds as many as it can get asks for one too many on purpose, so being out of
   // streams is an ordinary answer rather than something wrong
   if (index == WG_TCP_MAX) {
      if (!reportedAllInUse) logTrace(TAG "tcp: all %d streams are in use\n", WG_TCP_MAX);
      reportedAllInUse = 1;
      return -1;
   }

   // both the starting number and the port are random: a predictable pair lets someone else guess
   // their way into the conversation
   uint8_t randomBytes[6];
   if (getRandomBytes(randomBytes, sizeof randomBytes) != 0) return -1;
   uint32_t initialSequence = load32be(randomBytes);
   uint16_t localPort = (uint16_t)(FIRST_CHOSEN_PORT + (load16be(randomBytes + 4) % CHOSEN_PORT_COUNT));

   // two streams on one port would each take some of the other's segments, so a repeat is stepped past
   while (isLocalPortInUse(localPort))
      localPort = (uint16_t)(FIRST_CHOSEN_PORT + ((localPort - FIRST_CHOSEN_PORT + 1) % CHOSEN_PORT_COUNT));

   if (takeStreamBuffers(&streams[index]) != 0) return -1;

   openTcpConnection(&streams[index], address, port, localPort, initialSequence);
   streams[index].oldestSentMs = getMillisecondsNow();
   streamInUse[index] = 1;

   if (sendStreamControl(&streams[index], TCP_FLAG_SYN) != 0) {
      logError(TAG "tcp: tunnel is %s, so the connection was not attempted\n", getWgTunnelStateName(&tunnel));
      giveBackStreamBuffers(&streams[index]);
      streamInUse[index] = 0;
      return -1;
   }

   return index;
}

int isWgTcpConnecting(WgTcpSocket socket)
{
   if (socket < 0 || socket >= WG_TCP_MAX || !streamInUse[socket]) return 0;
   return streams[socket].state == WG_TCP_CONNECTING;
}

int isWgTcpFailed(WgTcpSocket socket)
{
   if (socket < 0 || socket >= WG_TCP_MAX || !streamInUse[socket]) return 1;
   return streams[socket].state == WG_TCP_FAILED;
}

WgTcpSocket connectWgTcp(uint32_t address, uint16_t port, int timeoutMs)
{
   WgTcpSocket socket = openWgTcp(address, port);
   if (socket < 0) return -1;

   for (int waited = 0; waited < timeoutMs; waited += STREAM_STEP_MS) {
      if (serviceWgNetwork(STREAM_STEP_MS) != 0) break;
      if (!isWgTcpConnecting(socket)) break;
   }

   if (streams[socket].state == WG_TCP_OPEN) return socket;

   if (streams[socket].state == WG_TCP_CONNECTING)
      logError(TAG "tcp: port %d did not answer within %dms\n", port, timeoutMs);

   closeWgTcp(socket);
   return -1;
}

int sendWgTcp(WgTcpSocket socket, const void *data, int length, int timeoutMs)
{
   if (socket < 0 || socket >= WG_TCP_MAX || !streamInUse[socket]) return -1;
   WgTcpConnection *stream = &streams[socket];
   if (stream->state != WG_TCP_OPEN) return -1;

   // take what fits, put it on its way, and only wait if there was no room for any of it
   for (int waited = 0; waited <= timeoutMs; waited += STREAM_STEP_MS) {
      int taken = writeTcpOutgoing(stream, (const uint8_t *)data, length);
      if (taken > 0) {
         flushStream(stream, getMillisecondsNow());
         return taken;
      }

      if (serviceWgNetwork(STREAM_STEP_MS) != 0) return -1;
      if (stream->state != WG_TCP_OPEN) return -1;
   }

   logError(TAG "tcp: nothing they had confirmed freed up within %dms\n", timeoutMs);
   return -1;
}

int recvWgTcp(WgTcpSocket socket, void *buffer, int capacity)
{
   if (socket < 0 || socket >= WG_TCP_MAX || !streamInUse[socket]) return -1;
   WgTcpConnection *stream = &streams[socket];
   if (stream->state == WG_TCP_FAILED) return -1;

   int take = readTcpIncoming(stream, (uint8_t *)buffer, capacity);

   // reading has made room, and they stay stopped until they are told about it
   if (shouldAnnounceWindow(stream)) sendStreamControl(stream, TCP_FLAG_ACK);

   return take;
}

int isWgTcpFinished(WgTcpSocket socket)
{
   if (socket < 0 || socket >= WG_TCP_MAX || !streamInUse[socket]) return 1;
   return streams[socket].remoteHasFinished && getTcpReadable(&streams[socket]) == 0;
}

void closeWgTcp(WgTcpSocket socket)
{
   if (socket < 0 || socket >= WG_TCP_MAX || !streamInUse[socket]) return;
   WgTcpConnection *stream = &streams[socket];

   reportedAllInUse = 0;   // there is room again, so the next shortage is worth saying

   // say goodbye once everything written has gone out, then give them a moment to agree
   if (stream->state == WG_TCP_OPEN) {
      for (int waited = 0; waited < CLOSE_WAIT_MS && stream->sendNext != stream->sendEnd; waited += STREAM_STEP_MS)
         if (serviceWgNetwork(STREAM_STEP_MS) != 0) break;

      stream->state = WG_TCP_CLOSING;
      stream->sendNext++;   // the goodbye takes a sequence number of its own
      sendStreamControl(stream, TCP_FLAG_FIN | TCP_FLAG_ACK);

      for (int waited = 0; waited < CLOSE_WAIT_MS && stream->state == WG_TCP_CLOSING; waited += STREAM_STEP_MS)
         if (serviceWgNetwork(STREAM_STEP_MS) != 0) break;
   }

   giveBackStreamBuffers(stream);
   memSet(stream, 0, sizeof *stream);
   streamInUse[socket] = 0;
}

// How much data a segment may carry if a packet of that many bytes gets through. Off the wire size
// come 28 bytes of outer IP and UDP, 16 of WireGuard header and a 16 byte tag; what is left is
// rounded down to a multiple of 16, because the tunnel pads to that, and then 20 bytes of IP and 20
// of TCP come off the inside. A different server, or a link that carries less, lands here.
static int getSegmentMaxForPacketLimit(int packetLimit)
{
   return (((packetLimit - 28 - 16 - 16) / 16) * 16) - 40;
}

// sizes worth knowing about: the common limits, and the ones either side of them
static const int probeSizes[] = { 548, 1028, 1280, 1380, 1412, 1420, 1440, 1468, 1500 };
#define PROBE_STEP_MS    200
#define PROBE_WAIT_MS   1500
#define PROBE_ATTEMPTS     3   // a lost packet must not be mistaken for a size that is too large

int measureWgPacketLimit(void)
{
   if (!isStarted) return -1;

   int largestThatWorked = -1;
   uint16_t sequence = 0;

   for (int index = 0; index < (int)(sizeof probeSizes / sizeof probeSizes[0]); index++) {
      int size = probeSizes[index];
      int replied = 0;

      for (int attempt = 0; attempt < PROBE_ATTEMPTS && !replied; attempt++) {
         sequence++;

         uint8_t packet[WG_PACKET_MAX];
         int packetLength = buildPingRequest(packet, sizeof packet, tunnel.config.tunnelAddress,
                                             tunnel.config.dnsAddress, PROBE_IDENTIFIER, sequence, size - 28);
         if (packetLength < 0) break;
         if (sendThroughWgTunnel(&tunnel, packet, packetLength) != 0) return -1;

         for (int waited = 0; waited < PROBE_WAIT_MS && !replied; waited += PROBE_STEP_MS) {
            uint8_t received[WG_PACKET_MAX];
            int length = serviceWgTunnel(&tunnel, received, sizeof received, PROBE_STEP_MS, 0);
            if (length < 0) return -1;
            if (length > 0 && isPingReplyFor(received, length, PROBE_IDENTIFIER, sequence)) replied = 1;
         }
      }

      logTrace(TAG "packet limit: %d bytes %s\n", size, replied ? "came back" : "did not come back in 3 tries");
      if (replied) largestThatWorked = size;
   }

   if (largestThatWorked > 0)
      logTrace(TAG "packet limit: streams will send up to %d bytes of data per segment\n",
              setTcpSegmentMax(getSegmentMaxForPacketLimit(largestThatWorked)));

   return largestThatWorked;
}

int resolveWgHost(const char *hostName, uint32_t *address, int timeoutMs)
{
   if (!isStarted) return -1;

   // the identifier ties an answer to the question that asked it, so a stale or forged reply for
   // some other name is ignored rather than believed
   uint8_t identifierBytes[2];
   if (getRandomBytes(identifierBytes, sizeof identifierBytes) != 0) return -1;
   uint16_t identifier = (uint16_t)((identifierBytes[0] << 8) | identifierBytes[1]);

   uint8_t query[WG_PACKET_MAX];
   int queryLength = buildDnsQuery(query, sizeof query, identifier, hostName);
   if (queryLength < 0) {
      logTrace(TAG "resolve: %s is not a usable host name\n", hostName);
      return -1;
   }

   WgSocket socket = openWgUdp(0);
   if (socket < 0) return -1;

   WgEndpoint nameServer;
   nameServer.address = tunnel.config.dnsAddress;
   nameServer.port = DNS_PORT;

   int result = -1;
   if (sendWgTo(socket, &nameServer, query, queryLength) != 0) {
      logTrace(TAG "resolve: tunnel is %s, so %s was not looked up\n", getWgTunnelStateName(&tunnel), hostName);
   } else {
      for (int waited = 0; waited < timeoutMs && result != 0; waited += RESOLVE_STEP_MS) {
         if (serviceWgNetwork(RESOLVE_STEP_MS) != 0) break;

         // A question is one packet, and nothing confirms it, so a lost one would cost the whole
         // lookup. Asking again costs nothing: the answer to whichever arrives carries the same
         // identifier, and a second answer is dropped with the socket.
         if (waited > 0 && (waited % RESOLVE_RESEND_MS) == 0 && sendWgTo(socket, &nameServer, query, queryLength) != 0)
            break;

         // an answer from anywhere else is someone guessing at our question, so where it came from
         // is checked as well as which question it answers
         uint8_t reply[WG_PACKET_MAX];
         WgEndpoint answeredBy;
         int replyLength = recvWgFrom(socket, &answeredBy, reply, sizeof reply);
         if (replyLength <= 0) continue;
         if (answeredBy.address != nameServer.address || answeredBy.port != nameServer.port) {
            logTrace(TAG "resolve: an answer for %s came from %u.%u.%u.%u port %d, not the name server, dropped\n",
                    hostName, answeredBy.address >> 24, (answeredBy.address >> 16) & 0xFF,
                    (answeredBy.address >> 8) & 0xFF, answeredBy.address & 0xFF, answeredBy.port);
            continue;
         }

         if (readDnsAnswer(reply, replyLength, identifier, address) == 0) result = 0;
         else logTrace(TAG "resolve: a %d byte reply for %s held no address\n", replyLength, hostName);
      }
      if (result != 0) logTrace(TAG "resolve: no answer for %s within %dms\n", hostName, timeoutMs);
   }

   closeWgUdp(socket);
   return result;
}
