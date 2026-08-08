#pragma once

// Talking to other holders of a torrent (BEP 3). Nothing here waits: a peer is moved along a step at
// a time by servicePeer, so a caller can hold several at once and use whichever answers. That
// matters more than it sounds, since a peer that is gone costs fifteen seconds to find out about,
// and a swarm is full of them.

#include <stdint.h>

#include "torrent-file.h"
#include "tracker.h"

#define PEER_BLOCK_LENGTH 16384   // what every client asks for and what most refuse to exceed
#define PEER_MESSAGE_MAX  (PEER_BLOCK_LENGTH + 256)   // room for a block, or a description piece and its header

// Which pieces a peer holds, one bit each, so 4 KB covers a torrent of 32768 pieces. A torrent with
// more than that is treated as though every peer holds all of it, which costs a wasted request.
#define PEER_HAVE_MAX 4096

typedef enum {
   PEER_CONNECTING,
   PEER_GREETING,   // handshake sent, waiting for theirs
   PEER_READY,
   PEER_DEAD
} PeerState;

typedef enum {
   PEER_EVENT_NONE,
   PEER_EVENT_BLOCK,         // blockBegin and blockLength say which part of the piece arrived
   PEER_EVENT_DESCRIPTION,   // a piece of the torrent's own description, for a magnet link
   PEER_EVENT_DIED
} PeerEvent;

typedef struct {
   PeerAddress address;
   PeerState   state;
   int       handle;
   int       choked;
   int       requests;        // blocks asked for and not yet answered
   uint64_t  quietSinceMs;    // when the last byte arrived, so a silent peer can be dropped
   uint64_t  readyAtMs;       // when the handshake finished, which is when it could have started serving
   uint64_t  spokeAtMs;       // when we last sent anything, so the peer is kept from dropping us

   uint8_t   message[PEER_MESSAGE_MAX];
   int       have;            // bytes of the message in hand
   int       need;            // bytes wanted before it can be acted on
   int       readingLength;   // the four bytes saying how long the message is come first

   int       discard;         // bytes of a message we have no use for, still to be read past

   int       extended;             // this peer speaks the extension protocol
   int       descriptionMessageId; // what it wants description requests numbered, 0 if it serves none
   int       descriptionSize;      // bytes of description it holds, 0 until it says

   uint8_t   piecesHeld[PEER_HAVE_MAX];   // one bit each, highest bit of a byte first
   int       piecesHeldKnown;             // 0 until it says, and then every piece has to be checked

   int       blockPiece;      // of the block PEER_EVENT_BLOCK is reporting
   int       blockBegin;      // where in the piece it goes; not used for a description piece
   int       blockLength;
   int       dataOffset;      // where the bytes sit inside the message just read
} PeerLink;

// Start connecting. Returns 0 when the attempt began, -1 when no connection could be started. The
// peer is not usable until servicePeer has taken it to PEER_READY.
int openPeer(PeerLink *link, const PeerAddress *address);
void closePeer(PeerLink *link);

// Move this peer along by whatever has arrived, without waiting. Call it in a loop until it says
// PEER_EVENT_NONE, then service the network and come back. A block's bytes are at getPeerBlock(link)
// and stay there only until the next call, so copy them out first.
PeerEvent servicePeer(PeerLink *link, const TorrentMeta *meta, const uint8_t *peerId);
const uint8_t *getPeerBlock(const PeerLink *link);

// ask for one block of a piece. 0 / -1.
int requestPeerBlock(PeerLink *link, int pieceIndex, int begin, int length);

// Whether this peer holds a piece. True until it says otherwise, since a peer that never sends its
// list is assumed to hold everything, which is what the protocol allows.
int peerHasPiece(const PeerLink *link, int pieceIndex);

// A torrent's description comes in 16 KB pieces too, over the extension protocol (BEP 9), which is
// how a magnet link turns into something downloadable. Ask only when descriptionMessageId is set.
// blockPiece says which piece arrived and blockLength how long it is.
int requestPeerDescription(PeerLink *link, int descriptionPiece);
