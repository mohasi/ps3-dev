#pragma once

// Asking a tracker for peers, over UDP (BEP 15). A torrent lists several trackers and any one of
// them can answer with the addresses of everyone else holding the content.
//
// It is two exchanges: a connect, which hands back an id that is good for a minute or so, then the
// announce itself. Both are matched by a random number we choose and checked against the sender,
// since UDP has no connection of its own and a reply meant for someone else could otherwise be
// taken as ours.

#include <stdint.h>

#include "sha1.h"

#define PEER_ID_LENGTH   20
// A tracker sends 50 by default and more when asked. Most of them will keep us choked, so the list
// has to be several times longer than the number of connections we hold.
#define TRACKER_PEER_MAX 128

typedef struct {
   uint32_t address;
   uint16_t port;
} PeerAddress;

typedef struct {
   int         seeders;
   int         leechers;
   int         intervalSeconds;   // how long to wait before asking again
   PeerAddress peers[TRACKER_PEER_MAX];
   int         peerCount;
} TrackerReply;

typedef enum {
   TRACKER_WORKING,
   TRACKER_ANSWERED,
   TRACKER_FAILED
} TrackerStatus;

#define TRACKER_REPLY_MAX (20 + TRACKER_PEER_MAX * 6)

typedef struct {
   char     host[128];
   uint32_t address;
   uint16_t port;
   int      handle;

   uint8_t        infoHash[SHA1_LENGTH];   // copied, so the caller may move the torrent it came from
   const uint8_t *peerId;
   uint16_t       listenPort;
   int64_t        left;

   uint64_t connectionId;
   uint32_t transactionId;
   int      connected;   // the connect exchange is behind us
   int      attempt;
   uint64_t sentAtMs;
   int      waitMs;      // before asking again, doubling each time it goes unanswered

   uint8_t      packet[TRACKER_REPLY_MAX];
   TrackerReply reply;
} TrackerAnnounce;

// Twenty bytes naming this client: "-SW0001-" and twelve random ones, as BEP 20 describes. It stays
// the same for as long as the app runs. Returns 0, or -1 when randomness could not be had.
int makePeerId(uint8_t *peerId);

// Start asking one tracker. left is how many bytes of the torrent are still missing, which is what
// tells the tracker whether we are a leecher or a seeder. The info hash is copied; peerId is not,
// and has to outlive the request. Returns 0, or -1 when the address is not a udp one or the name
// could not be looked up. Looking the name up is the one part that waits.
int startTrackerAnnounce(TrackerAnnounce *announce, const char *trackerUrl, const uint8_t *infoHash,
                         const uint8_t *peerId, uint16_t listenPort, int64_t left);

// Move it along by whatever has arrived, without waiting. The peers are in announce->reply once it
// says TRACKER_ANSWERED.
TrackerStatus serviceTrackerAnnounce(TrackerAnnounce *announce);
void stopTrackerAnnounce(TrackerAnnounce *announce);
