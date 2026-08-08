// tracker - BEP 15, the UDP way of asking a tracker who else is holding a torrent.

#include "tracker.h"

#include "dbg.h"
#include "string-utilities.h"
#include "torrent-net.h"

#define TAG "[bt] "

#define CONNECT_MAGIC 0x41727101980ULL   // BEP 15: the constant every connect request carries
#define ACTION_CONNECT  0
#define ACTION_ANNOUNCE 1
#define ACTION_ERROR    3
#define EVENT_STARTED   2

#define ANNOUNCE_REQUEST_LENGTH 98
#define CONNECT_REQUEST_LENGTH  16

// BEP 15 asks for 15 seconds doubling eight times, which is over an hour. A person is waiting here,
// so a tracker that has not answered in half a minute is left for the next one in the list.
#define ATTEMPT_MAX   3
#define FIRST_WAIT_MS 2000   // doubling each try, so a tracker that is gone costs 14s, not 28s
#define RESOLVE_TIMEOUT_MS 5000

static uint16_t readBe16(const uint8_t *bytes)
{
   return (uint16_t)((bytes[0] << 8) | bytes[1]);
}

static uint32_t readBe32(const uint8_t *bytes)
{
   return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void writeBe32(uint8_t *out, uint32_t value)
{
   out[0] = (uint8_t)(value >> 24);
   out[1] = (uint8_t)(value >> 16);
   out[2] = (uint8_t)(value >> 8);
   out[3] = (uint8_t)value;
}

static void writeBe64(uint8_t *out, uint64_t value)
{
   writeBe32(out, (uint32_t)(value >> 32));
   writeBe32(out + 4, (uint32_t)value);
}

// "udp://host:port/announce". the path is not used by the udp protocol.
static int parseTrackerUrl(const char *url, char *host, int hostCapacity, uint16_t *port)
{
   if (!startsWith(url, "udp://")) return -1;

   const char *text = url + 6;
   int length = 0;
   while (text[length] && text[length] != ':' && text[length] != '/') length++;
   if (length == 0 || length >= hostCapacity) return -1;

   memCopy(host, text, length);
   host[length] = 0;

   *port = 80;   // what a URL with no port means, though a tracker nearly always gives one
   if (text[length] != ':') return 0;

   int64_t value = 0;
   for (const char *digit = text + length + 1; *digit >= '0' && *digit <= '9'; digit++) value = value * 10 + (*digit - '0');
   if (value <= 0 || value > 65535) return -1;

   *port = (uint16_t)value;
   return 0;
}

static int getTransactionId(uint32_t *transactionId)
{
   uint8_t bytes[4];
   if (getTorrentNetwork()->getRandom(bytes, sizeof bytes) != 0) return -1;

   *transactionId = readBe32(bytes);
   return 0;
}

int makePeerId(uint8_t *peerId)
{
   const TorrentNetwork *network = getTorrentNetwork();
   if (!network) return -1;

   // BEP 20: two letters for the client and four digits for its version, then anything
   memCopy(peerId, "-SW0001-", 8);
   return network->getRandom(peerId + 8, PEER_ID_LENGTH - 8);
}

// the first request: an id to quote in the announce, which is how a tracker tells a real request
// from one with a forged return address
static int sendConnect(TrackerAnnounce *announce)
{
   if (getTransactionId(&announce->transactionId) != 0) return -1;

   uint8_t request[CONNECT_REQUEST_LENGTH];
   writeBe64(request, CONNECT_MAGIC);
   writeBe32(request + 8, ACTION_CONNECT);
   writeBe32(request + 12, announce->transactionId);

   return getTorrentNetwork()->sendUdp(announce->handle, announce->address, announce->port, request, sizeof request);
}

static int sendAnnounce(TrackerAnnounce *announce)
{
   if (getTransactionId(&announce->transactionId) != 0) return -1;

   uint8_t request[ANNOUNCE_REQUEST_LENGTH];
   memSet(request, 0, sizeof request);
   writeBe64(request, announce->connectionId);
   writeBe32(request + 8, ACTION_ANNOUNCE);
   writeBe32(request + 12, announce->transactionId);
   memCopy(request + 16, announce->infoHash, SHA1_LENGTH);
   memCopy(request + 36, announce->peerId, PEER_ID_LENGTH);
   writeBe64(request + 64, (uint64_t)announce->left);
   writeBe32(request + 80, EVENT_STARTED);
   writeBe32(request + 92, 0xFFFFFFFF);   // as many peers as it cares to send
   request[96] = (uint8_t)(announce->listenPort >> 8);
   request[97] = (uint8_t)announce->listenPort;

   return getTorrentNetwork()->sendUdp(announce->handle, announce->address, announce->port, request, sizeof request);
}

static int sendWhicheverIsDue(TrackerAnnounce *announce)
{
   announce->sentAtMs = getTorrentNetwork()->getNowMs();
   announce->attempt++;

   return announce->connected ? sendAnnounce(announce) : sendConnect(announce);
}

static void readPeers(TrackerAnnounce *announce, int length)
{
   TrackerReply *reply = &announce->reply;

   reply->intervalSeconds = (int)readBe32(announce->packet + 8);
   reply->leechers = (int)readBe32(announce->packet + 12);
   reply->seeders = (int)readBe32(announce->packet + 16);

   for (int offset = 20; offset + 6 <= length && reply->peerCount < TRACKER_PEER_MAX; offset += 6) {
      reply->peers[reply->peerCount].address = readBe32(announce->packet + offset);
      reply->peers[reply->peerCount].port = readBe16(announce->packet + offset + 4);
      reply->peerCount++;
   }
}

// what one packet from the tracker means
static TrackerStatus takeAnswer(TrackerAnnounce *announce, int length)
{
   if (readBe32(announce->packet) == ACTION_ERROR) {
      char message[128];
      int take = length - 8 < (int)sizeof message - 1 ? length - 8 : (int)sizeof message - 1;
      memCopy(message, announce->packet + 8, take);
      message[take] = 0;
      logTrace(TAG "tracker: %s refused us, %s\n", announce->host, message);
      return TRACKER_FAILED;
   }

   if (!announce->connected) {
      if (length < 16 || readBe32(announce->packet) != ACTION_CONNECT) return TRACKER_WORKING;

      announce->connectionId = ((uint64_t)readBe32(announce->packet + 8) << 32) | readBe32(announce->packet + 12);
      announce->connected = 1;
      announce->attempt = 0;
      announce->waitMs = FIRST_WAIT_MS;
      return sendWhicheverIsDue(announce) == 0 ? TRACKER_WORKING : TRACKER_FAILED;
   }

   if (length < 20 || readBe32(announce->packet) != ACTION_ANNOUNCE) return TRACKER_WORKING;

   readPeers(announce, length);
   logTrace(TAG "tracker: %s:%d, %d peers, %d seeders, %d leechers, ask again in %ds\n", announce->host, announce->port,
           announce->reply.peerCount, announce->reply.seeders, announce->reply.leechers,
           announce->reply.intervalSeconds);
   return TRACKER_ANSWERED;
}

int startTrackerAnnounce(TrackerAnnounce *announce, const char *trackerUrl, const uint8_t *infoHash,
                         const uint8_t *peerId, uint16_t listenPort, int64_t left)
{
   const TorrentNetwork *network = getTorrentNetwork();
   if (!network) {
      logError(TAG "tracker: no network was lent to the library\n");
      return -1;
   }

   memSet(announce, 0, sizeof *announce);
   announce->handle = -1;
   memCopy(announce->infoHash, infoHash, SHA1_LENGTH);
   announce->peerId = peerId;
   announce->listenPort = listenPort;
   announce->left = left;
   announce->waitMs = FIRST_WAIT_MS;

   if (parseTrackerUrl(trackerUrl, announce->host, sizeof announce->host, &announce->port) != 0) {
      logTrace(TAG "tracker: %s is not a udp address, skipped\n", trackerUrl);
      return -1;
   }

   if (network->resolve(announce->host, &announce->address, RESOLVE_TIMEOUT_MS) != 0) {
      logTrace(TAG "tracker: %s could not be looked up\n", announce->host);
      return -1;
   }

   announce->handle = network->openUdp();
   if (announce->handle < 0) {
      logError(TAG "tracker: no port was free to ask from\n");
      return -1;
   }

   if (sendWhicheverIsDue(announce) != 0) {
      stopTrackerAnnounce(announce);
      return -1;
   }

   return 0;
}

void stopTrackerAnnounce(TrackerAnnounce *announce)
{
   if (announce->handle < 0) return;

   getTorrentNetwork()->closeUdp(announce->handle);
   announce->handle = -1;
}

TrackerStatus serviceTrackerAnnounce(TrackerAnnounce *announce)
{
   const TorrentNetwork *network = getTorrentNetwork();
   if (announce->handle < 0) return TRACKER_FAILED;

   // section: anything that arrived, if it is really for us
   uint32_t fromAddress = 0;
   uint16_t fromPort = 0;
   int length = network->receiveUdp(announce->handle, &fromAddress, &fromPort, announce->packet,
                                    sizeof announce->packet);

   if (length >= 8 && fromAddress == announce->address && fromPort == announce->port &&
       readBe32(announce->packet + 4) == announce->transactionId) {
      TrackerStatus status = takeAnswer(announce, length);
      if (status != TRACKER_WORKING) stopTrackerAnnounce(announce);
      return status;
   }

   // section: ask again when it has gone unanswered for long enough
   if (network->getNowMs() - announce->sentAtMs < (uint64_t)announce->waitMs) return TRACKER_WORKING;

   if (announce->attempt >= ATTEMPT_MAX) {
      logTrace(TAG "tracker: %s:%d did not answer\n", announce->host, announce->port);
      stopTrackerAnnounce(announce);
      return TRACKER_FAILED;
   }

   announce->waitMs *= 2;
   if (sendWhicheverIsDue(announce) != 0) {
      stopTrackerAnnounce(announce);
      return TRACKER_FAILED;
   }

   return TRACKER_WORKING;
}

