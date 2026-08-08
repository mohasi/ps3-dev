// torrent-engine - the downloads, serviced a slice at a time.
//
// One torrent downloads at a time and the others wait, which is what keeps the memory to a single
// piece. Everything the active torrent needs beyond its own description lives here rather than in
// the slot, for the same reason.

#include "torrent-engine.h"

#include "dbg.h"
#include "magnet.h"
#include "path.h"
#include "peer.h"
#include "sha1.h"
#include "string-utilities.h"
#include "torrent-net.h"
#include "torrent-storage.h"

#define TAG "[bt] "

// Most peers keep us choked, so the number that will actually serve is a fraction of the number we
// hold. Each connection costs about 21 KB, so the ceiling is what the app can spare rather than
// anything the network minds.
#define PEER_ACTIVE_MAX 48
#define PEER_START_COUNT 16      // opened at once when a download starts
#define PEER_GROW_STEP    8      // more connections opened when too few of them are serving
#define PEER_GROW_MS  10000
#define SERVING_WANTED    8      // peers serving at once that would fill the line
#define CHOKED_ROTATE_MS 45000   // a peer that has kept us choked this long is swapped for another
#define IN_FLIGHT_MAX   24       // blocks asked of one peer at a time, which is what keeps the line busy
#define PIECE_WORK_MAX   8       // pieces collected at once, so one slow peer cannot hold up the rest
#define SECOND_ASK_MAX   8       // blocks an idle peer may ask for that someone else already owes
#define BLOCK_MAX     1024       // enough for a 16 MB piece, larger than any torrent uses
#define PIECE_TIMEOUT_MS 120000   // a seeder can take a minute to get round to unchoking us
#define BLOCK_TIMEOUT_MS  12000   // silence from a peer that owes us blocks, before it is dropped
#define SCAN_PER_CALL     1      // pieces checked against the disk per call, so resuming does not stall the app
#define MISSES_BEFORE_TRUSTING 4 // checks that find nothing before the rest is taken to be missing too
#define PIECE_BITMAP_MAX 4096    // pieces marked taken, one bit each: 32768 of them, or 64 GB at 2 MB a piece
#define LISTEN_PORT    6881      // where a client is expected to be reachable, by long convention
#define SECOND_MS      1000

// a description comes in 16 KB pieces like everything else, so this covers one of 4 MB, which is far
// larger than any torrent's
#define DESCRIPTION_PIECE_MAX 256
#define DESCRIBE_TIMEOUT_MS 60000

#define TRACKER_RETRY_MS   30000   // before asking every tracker again
#define TRACKER_ROUNDS_MAX     5

typedef struct {
   TorrentMeta   meta;
   TorrentStatus status;
   char          contentPath[MAX_PATH_LEN];

   TrackerAnnounce announce;
   TrackerReply    peers;
   int             trackerIndex;
   int             trackerRounds;    // times every tracker has been tried and none answered
   uint64_t        askAgainAtMs;     // not before this, so a busy tracker is not hammered

   int      nextPiece;      // where to start looking for the next piece to take on
   int      askedAboutDisk; // whether the check below has been made yet
   int      resuming;       // content was already there, so each piece is worth checking before asking
   int      missesInARow;   // checks that found nothing, which is how the checking knows to stop
   char     reason[TORRENT_REASON_MAX];   // why it failed, for a screen to show
   uint8_t  taken[PIECE_BITMAP_MAX];   // pieces done or in hand, one bit each
   uint64_t lastBlockAtMs;  // when anything last arrived, so a swarm that stops serving is given up on

   // filled in only for a torrent that started as a magnet link
   uint8_t *description;
   int      descriptionCapacity;
   int      descriptionSize;
   int      descriptionPieces;
   int      descriptionLeft;
   uint8_t  descriptionDone[DESCRIPTION_PIECE_MAX];
   uint64_t describingSinceMs;

   int      piecesDone;
   int64_t  bytesDone;
   int      bytesThisSecond;
   int      bytesPerSecond;
   uint64_t secondStartedMs;
} TorrentSlot;

// One piece being collected. Several are held at once so every peer has something to send, and a
// peer that is slow on one piece does not stop the others from filling.
typedef struct {
   int      piece;               // which one, -1 when this slot is free
   int      length;
   uint8_t *data;                // its own share of the app's piece buffer
   uint8_t  done[BLOCK_MAX];
   int8_t   owner[BLOCK_MAX];    // which peer was asked for it, -1 for nobody
   int      count;
   int      left;
} PieceWork;

static TorrentSlot slots[TORRENT_SLOT_MAX];
static int slotCount;
static int activeSlot = -1;

static PeerLink  links[PEER_ACTIVE_MAX];
static PieceWork work[PIECE_WORK_MAX];
static int workCount;            // how many the piece buffer holds for the torrent being downloaded
static int plannedPieceLength;   // what that layout was worked out for
static int peerCursor;
static int peerAttemptsLeft;
static int peerTarget = PEER_START_COUNT;   // connections being held now, which grows while few serve
static uint64_t growPeersAtMs;

static uint8_t peerId[PEER_ID_LENGTH];
static const char *downloads;
static uint8_t *piece;
static int pieceCapacity;

// section: peers and blocks, all of which belong to the torrent being downloaded now

// How many pieces can be collected at once: as many as the app's buffer holds, up to the limit. A
// torrent with pieces as large as the whole buffer gets one. Laying the buffer out again would lose
// whatever is half collected, so it is only done when the piece size is not the one already planned.
static void planWork(int pieceLength)
{
   if (pieceLength == plannedPieceLength) return;

   plannedPieceLength = pieceLength;
   workCount = pieceCapacity / pieceLength;
   if (workCount > PIECE_WORK_MAX) workCount = PIECE_WORK_MAX;
   if (workCount < 1) workCount = 1;

   int share = pieceCapacity / workCount;
   for (int index = 0; index < workCount; index++) {
      work[index].piece = -1;
      work[index].data = piece + index * share;
   }
}

// a failure a person can read, kept beside the status so a screen never has to guess
static void failTorrent(TorrentSlot *torrent, const char *reason)
{
   torrent->status = TORRENT_FAILED;
   strCopy(torrent->reason, TORRENT_REASON_MAX, reason);
}

static void clearPieceTaken(TorrentSlot *torrent, int piece);

// Nothing half collected survives a change of torrent, since the buffer is laid out for one at a
// time. What was in hand goes back on offer, or those pieces would never be asked for again.
static void freeAllWork(TorrentSlot *torrent)
{
   for (int index = 0; index < PIECE_WORK_MAX; index++) {
      if (torrent && work[index].piece >= 0) clearPieceTaken(torrent, work[index].piece);
      work[index].piece = -1;
   }

   plannedPieceLength = 0;
}

static void startWork(PieceWork *slot, int pieceIndex, int pieceLength)
{
   slot->piece = pieceIndex;
   slot->length = pieceLength;
   slot->count = (pieceLength + PEER_BLOCK_LENGTH - 1) / PEER_BLOCK_LENGTH;
   slot->left = slot->count;

   memSet(slot->done, 0, slot->count);
   memSet(slot->owner, 0xFF, slot->count);   // -1 in every entry: nobody has been asked yet
}

static PieceWork *findWork(int pieceIndex)
{
   for (int index = 0; index < workCount; index++)
      if (work[index].piece == pieceIndex) return &work[index];

   return NULL;
}

// hand back what a peer was holding when it died, so someone else can be asked
static void releaseBlocks(int peerIndex)
{
   for (int index = 0; index < workCount; index++)
      for (int block = 0; block < work[index].count; block++)
         if (work[index].owner[block] == peerIndex) work[index].owner[block] = -1;
}

static int getBlockLength(int block, int pieceLength)
{
   int begin = block * PEER_BLOCK_LENGTH;
   return pieceLength - begin < PEER_BLOCK_LENGTH ? pieceLength - begin : PEER_BLOCK_LENGTH;
}

static void closeAllPeers(void)
{
   for (int index = 0; index < PEER_ACTIVE_MAX; index++) {
      releaseBlocks(index);   // whatever they owed is nobody's now, or it would never be asked for again
      closePeer(&links[index]);
      links[index].state = PEER_DEAD;
   }
}

// fill any empty connection from the tracker's list, moving through it as peers turn out to be gone
static void fillPeerSlots(const TrackerReply *peers)
{
   for (int index = 0; index < peerTarget && peerAttemptsLeft > 0; index++) {
      if (links[index].state != PEER_DEAD) continue;
      if (peerCursor >= peers->peerCount) peerCursor = 0;

      int started = openPeer(&links[index], &peers->peers[peerCursor]);
      peerCursor++;
      peerAttemptsLeft--;

      // no room in the tunnel, or that address was refused outright: leave the rest for the next pass
      if (started != 0) return;
   }
}

static int getLivePeerCount(void)
{
   int live = 0;
   for (int index = 0; index < PEER_ACTIVE_MAX; index++)
      if (links[index].state != PEER_DEAD) live++;

   return live;
}

// peers that have said they will send us something, which is what the download rate is made of
static int getServingPeerCount(void)
{
   int serving = 0;
   for (int index = 0; index < PEER_ACTIVE_MAX; index++)
      if (links[index].state == PEER_READY && !links[index].choked) serving++;

   return serving;
}

// A peer only serves a few of the clients it is talking to, so the number that will serve us is not
// something we can ask for: we hold more connections until enough of them are serving. It is never
// wound back down, since a peer that is sending is worth keeping whatever the others are doing.
static void adjustPeerTarget(TorrentSlot *torrent)
{
   uint64_t now = getTorrentNetwork()->getNowMs();
   if (now < growPeersAtMs) return;

   growPeersAtMs = now + PEER_GROW_MS;
   peerAttemptsLeft = torrent->peers.peerCount;   // one more sweep of the list before the next tick

   if (getServingPeerCount() >= SERVING_WANTED || peerTarget >= PEER_ACTIVE_MAX) return;

   // Not even the connections we already asked for are open: either the tunnel has no more streams
   // or the addresses are dead, and asking for more of either buys nothing.
   if (getLivePeerCount() < peerTarget - PEER_GROW_STEP) return;

   peerTarget += PEER_GROW_STEP;
   if (peerTarget > PEER_ACTIVE_MAX) peerTarget = PEER_ACTIVE_MAX;
}

// A peer that takes a request and never answers holds up the whole piece, and offering the block
// again is no use while that same peer is still there to take it: it is dropped, its blocks go back
// on offer, and the slot fills with someone else.
static void dropPeer(int peerIndex)
{
   releaseBlocks(peerIndex);
   closePeer(&links[peerIndex]);
   links[peerIndex].state = PEER_DEAD;
}

// A peer that was asked for blocks and has then sent nothing at all for a while is holding them
// hostage: nobody else may be asked for a block someone already has. Judged on the peer's silence
// rather than any one block, so a peer that is steadily sending is left alone however slow it is.
static void dropPeersThatDoNotAnswer(void)
{
   uint64_t now = getTorrentNetwork()->getNowMs();

   for (int index = 0; index < PEER_ACTIVE_MAX; index++) {
      PeerLink *link = &links[index];
      if (link->state != PEER_READY || link->requests == 0) continue;
      if (now - link->quietSinceMs < BLOCK_TIMEOUT_MS) continue;

      char address[16];
      formatIpv4(address, sizeof address, link->address.address);
      logTrace(TAG "download: %s:%d took a request and sent nothing, dropped\n", address, link->address.port);

      dropPeer(index);
   }
}

// A peer that has kept us choked since it connected is holding a slot another address might serve
// from. Judged on how long ago it shook hands rather than on its silence: a peer can keep us choked
// while sending a steady stream of messages saying what it now holds.
static void rotatePeersThatKeepUsChoked(void)
{
   uint64_t now = getTorrentNetwork()->getNowMs();

   for (int index = 0; index < PEER_ACTIVE_MAX; index++) {
      PeerLink *link = &links[index];
      if (link->state != PEER_READY || !link->choked) continue;
      if (now - link->readyAtMs < CHOKED_ROTATE_MS) continue;

      dropPeer(index);
   }
}

// ask one peer for as much as it will hold, taking from every piece in hand that it has
static void askForBlocks(int peerIndex)
{
   PeerLink *link = &links[peerIndex];
   if (link->state != PEER_READY || link->choked) return;

   for (int index = 0; index < workCount && link->requests < IN_FLIGHT_MAX; index++) {
      PieceWork *slot = &work[index];
      if (slot->piece < 0 || !peerHasPiece(link, slot->piece)) continue;

      for (int block = 0; block < slot->count && link->requests < IN_FLIGHT_MAX; block++) {
         if (slot->done[block] || slot->owner[block] >= 0) continue;

         if (requestPeerBlock(link, slot->piece, block * PEER_BLOCK_LENGTH, getBlockLength(block, slot->length)) != 0)
            return;

         slot->owner[block] = (int8_t)peerIndex;
      }
   }

   // Nothing was free to ask for, so this peer would sit idle while a slower one takes its time over
   // blocks it already owes. Asking it for a few of those as well costs a little traffic and takes
   // the wait down to whichever of the two answers first.
   if (link->requests > 0) return;

   for (int index = 0; index < workCount; index++) {
      PieceWork *slot = &work[index];
      if (slot->piece < 0 || !peerHasPiece(link, slot->piece)) continue;

      for (int block = 0; block < slot->count && link->requests < SECOND_ASK_MAX; block++) {
         if (slot->done[block] || slot->owner[block] == peerIndex) continue;

         if (requestPeerBlock(link, slot->piece, block * PEER_BLOCK_LENGTH, getBlockLength(block, slot->length)) != 0)
            return;
      }
   }
}

static void takeBlock(int peerIndex, TorrentSlot *torrent)
{
   const PeerLink *link = &links[peerIndex];

   PieceWork *slot = findWork(link->blockPiece);
   if (!slot) return;   // a block of a piece we have already finished with

   // the offset and the length came off the wire, so a negative one has to be refused before it is
   // used as an index: it would pass every test that only looks for something too large
   if (link->blockBegin < 0 || link->blockLength <= 0) return;

   int block = link->blockBegin / PEER_BLOCK_LENGTH;
   if (link->blockBegin % PEER_BLOCK_LENGTH != 0 || block >= slot->count) return;
   if (link->blockBegin + link->blockLength > slot->length || slot->done[block]) return;

   memCopy(slot->data + link->blockBegin, getPeerBlock(link), link->blockLength);
   slot->done[block] = 1;
   slot->owner[block] = -1;
   slot->left--;

   // counted here rather than when the piece is done, or the speed reads zero for whole pieces at a time
   torrent->bytesThisSecond += link->blockLength;
   torrent->lastBlockAtMs = getTorrentNetwork()->getNowMs();
}

// section: the description, which a magnet link does not carry

static void storeDescriptionPiece(TorrentSlot *torrent, const PeerLink *link)
{
   if (link->blockPiece < 0 || link->blockPiece >= torrent->descriptionPieces) return;
   if (link->blockLength <= 0) return;

   int begin = link->blockPiece * PEER_BLOCK_LENGTH;
   if (begin + link->blockLength > torrent->descriptionSize) return;
   if (torrent->descriptionDone[link->blockPiece]) return;

   memCopy(torrent->description + begin, getPeerBlock(link), link->blockLength);
   torrent->descriptionDone[link->blockPiece] = 1;
   torrent->descriptionLeft--;
}

// a peer only says how long the description is once its extension handshake has arrived
static void noteDescriptionSize(TorrentSlot *torrent, const PeerLink *link)
{
   if (torrent->descriptionSize > 0 || link->descriptionSize <= 0) return;

   if (link->descriptionSize > torrent->descriptionCapacity) {
      logError(TAG "describe: it needs %d KB of description, more than the buffer holds\n",
               link->descriptionSize / 1024);
      failTorrent(torrent, "it is too large to describe");
      return;
   }

   torrent->descriptionSize = link->descriptionSize;
   torrent->descriptionPieces = (link->descriptionSize + PEER_BLOCK_LENGTH - 1) / PEER_BLOCK_LENGTH;
   torrent->descriptionLeft = torrent->descriptionPieces;
   memSet(torrent->descriptionDone, 0, torrent->descriptionPieces);

   logTrace(TAG "describe: %s is %d bytes, in %d pieces\n", torrent->meta.name, torrent->descriptionSize,
           torrent->descriptionPieces);
}

static void askForDescription(int index, TorrentSlot *torrent)
{
   PeerLink *link = &links[index];
   if (link->state != PEER_READY || link->descriptionMessageId <= 0 || torrent->descriptionPieces == 0) return;

   for (int piece = 0; piece < torrent->descriptionPieces && link->requests < IN_FLIGHT_MAX; piece++) {
      if (torrent->descriptionDone[piece]) continue;
      if (requestPeerDescription(link, piece) != 0) return;
   }
}

// one pass over every peer: ask for what is free, take what arrived
static void servicePeers(TorrentSlot *torrent)
{
   int describing = torrent->status == TORRENT_DESCRIBING;
   fillPeerSlots(&torrent->peers);

   for (int index = 0; index < PEER_ACTIVE_MAX; index++) {
      if (describing) {
         noteDescriptionSize(torrent, &links[index]);
         askForDescription(index, torrent);
      } else {
         askForBlocks(index);
      }

      PeerEvent event;
      while ((event = servicePeer(&links[index], &torrent->meta, peerId)) != PEER_EVENT_NONE) {
         if (event == PEER_EVENT_BLOCK) takeBlock(index, torrent);
         else if (event == PEER_EVENT_DESCRIPTION) storeDescriptionPiece(torrent, &links[index]);
         else if (event == PEER_EVENT_DIED && !describing) releaseBlocks(index);
      }
   }
}

static int isPieceRight(const TorrentMeta *meta, const PieceWork *slot)
{
   uint8_t hash[SHA1_LENGTH];
   hashSha1(hash, slot->data, slot->length);

   for (int index = 0; index < SHA1_LENGTH; index++)
      if (hash[index] != meta->pieceHashes[slot->piece * SHA1_LENGTH + index]) return 0;

   return 1;
}

// section: one torrent's progress through its pieces

static void tickSpeed(TorrentSlot *torrent)
{
   uint64_t elapsed = getTorrentNetwork()->getNowMs() - torrent->secondStartedMs;
   if (elapsed < SECOND_MS) return;

   // smoothed over about four seconds, because blocks arrive in bursts and a bare per-second count
   // spends most of its time reading zero
   int measured = (int)((int64_t)torrent->bytesThisSecond * SECOND_MS / (int64_t)elapsed);
   torrent->bytesPerSecond = (torrent->bytesPerSecond * 3 + measured) / 4;
   torrent->bytesThisSecond = 0;
   torrent->secondStartedMs = getTorrentNetwork()->getNowMs();
}

static void finishTorrent(TorrentSlot *torrent)
{
   closeAllPeers();
   torrent->status = TORRENT_FINISHED;

   if (torrent->piecesDone == torrent->meta.pieceCount)
      finishTorrentStorage(&torrent->meta, downloads, torrent->contentPath);

   logTrace(TAG "download: %s has finished\n", torrent->meta.name);
   logInfo(TAG "download: finished, %d of %d pieces, %lld KB on disk\n", torrent->piecesDone,
           torrent->meta.pieceCount, (long long)(torrent->bytesDone / 1024));
}

static void waitAndAskAgain(TorrentSlot *torrent);   // a stalled download goes back to the trackers

static int isPieceTaken(const TorrentSlot *torrent, int piece)
{
   if (piece / 8 >= PIECE_BITMAP_MAX) return 1;   // past what the map covers: never taken on
   return (torrent->taken[piece / 8] & (0x80 >> (piece % 8))) != 0;
}

static void markPieceTaken(TorrentSlot *torrent, int piece)
{
   if (piece / 8 < PIECE_BITMAP_MAX) torrent->taken[piece / 8] |= 0x80 >> (piece % 8);
}

// back on offer: a piece that was in hand when the work was thrown away was never collected
static void clearPieceTaken(TorrentSlot *torrent, int piece)
{
   if (piece >= 0 && piece / 8 < PIECE_BITMAP_MAX) torrent->taken[piece / 8] &= ~(0x80 >> (piece % 8));
}

static int isPieceHeldByAnyPeer(int piece)
{
   for (int index = 0; index < PEER_ACTIVE_MAX; index++)
      if (links[index].state == PEER_READY && peerHasPiece(&links[index], piece)) return 1;

   return 0;
}

// The next piece worth taking on: one the peers we are talking to can actually serve, looked for
// from where the last search left off so the file still fills roughly front to back. Falls back to
// the first piece nobody has claimed, which is what happens before any peer has said what it holds.
static int chooseNextPiece(TorrentSlot *torrent)
{
   int wanted = torrent->meta.pieceCount;
   int fallback = -1;

   for (int offset = 0; offset < wanted; offset++) {
      int piece = (torrent->nextPiece + offset) % wanted;
      if (isPieceTaken(torrent, piece)) continue;

      if (fallback < 0) fallback = piece;
      if (!isPieceHeldByAnyPeer(piece)) continue;

      torrent->nextPiece = (piece + 1) % wanted;
      return piece;
   }

   if (fallback >= 0) torrent->nextPiece = (fallback + 1) % wanted;
   return fallback;
}

// Whether any of this torrent's content is already on disk. Reading a piece back and hashing it
// costs as much as downloading it does, so it is only worth doing when there is something to find.
static int isAnythingOnDisk(const TorrentSlot *torrent)
{
   char path[MAX_PATH_LEN];

   for (int index = 0; index < torrent->meta.fileCount; index++) {
      joinPath(path, sizeof path, torrent->contentPath, torrent->meta.files[index].path);
      if (fileExists(path)) return 1;
   }

   return 0;
}

// Take on more pieces while there is room. Only a few are checked against the disk per call, so a
// resumed torrent does not hold the app up.
static void fillWorkSlots(TorrentSlot *torrent)
{
   int checked = 0;

   if (!torrent->askedAboutDisk) {
      torrent->askedAboutDisk = 1;
      torrent->resuming = isAnythingOnDisk(torrent);
      if (torrent->resuming) logTrace(TAG "download: %s was started before, checking what is here\n",
                                     torrent->meta.name);
   }

   for (int index = 0; index < workCount; index++) {
      if (work[index].piece >= 0) continue;
      if (torrent->resuming && checked >= SCAN_PER_CALL) break;   // the rest of the checks wait for the next pass

      int pieceIndex = chooseNextPiece(torrent);
      if (pieceIndex < 0) break;   // every piece is either done or in hand

      int pieceLength = getPieceLength(&torrent->meta, pieceIndex);
      if (pieceLength <= 0) break;   // the description does not add up, so there is nothing to ask for

      markPieceTaken(torrent, pieceIndex);
      checked++;

      // one already on disk from an earlier run costs a hash check and nothing else
      if (torrent->resuming &&
          isPieceOnDisk(&torrent->meta, torrent->contentPath, pieceIndex, work[index].data, pieceLength)) {
         torrent->bytesDone += pieceLength;
         torrent->piecesDone++;
         torrent->missesInARow = 0;
         index--;   // the slot is still free, so try another piece in it
         continue;
      }

      // What was there has run out, and checking every piece of what is not costs as much as
      // downloading it. Anything still on disk past this point is simply fetched again.
      if (torrent->resuming && ++torrent->missesInARow >= MISSES_BEFORE_TRUSTING) {
         torrent->resuming = 0;
         logInfo(TAG "download: %d pieces were already here\n", torrent->piecesDone);
      }

      startWork(&work[index], pieceIndex, pieceLength);
   }

}

static void finishPiece(TorrentSlot *torrent, PieceWork *slot)
{
   if (!isPieceRight(&torrent->meta, slot)) {
      logError(TAG "download: piece %d arrived damaged and was thrown away\n", slot->piece);
      startWork(slot, slot->piece, slot->length);   // ask for the whole of it again
      return;
   }

   if (writeTorrentPiece(&torrent->meta, torrent->contentPath, slot->piece, slot->data, slot->length) != 0) {
      logError(TAG "download: piece %d could not be written\n", slot->piece);
      failTorrent(torrent, "a piece could not be written to disk");
      closeAllPeers();
      return;
   }

   torrent->bytesDone += slot->length;
   torrent->piecesDone++;
   torrent->trackerRounds = 0;   // it is being served, so earlier stalls should not count against it
   slot->piece = -1;
}

static int isAnyWorkBusy(void)
{
   for (int index = 0; index < workCount; index++)
      if (work[index].piece >= 0) return 1;

   return 0;
}

// section: pace - one line saying where the download is and, when it is slow, what is holding it up.
// Half a minute apart, which is often enough to see a stall and rare enough not to fill the log.

#define PACE_EVERY_MS 30000

static uint64_t nextPaceMs;

static void reportPace(const TorrentSlot *torrent)
{
   uint64_t now = getTorrentNetwork()->getNowMs();
   if (now < nextPaceMs) return;

   nextPaceMs = now + PACE_EVERY_MS;

   int shookHands = 0, serving = 0, waiting = 0;
   for (int index = 0; index < PEER_ACTIVE_MAX; index++) {
      if (links[index].state != PEER_READY) continue;

      shookHands++;
      if (!links[index].choked) serving++;
      waiting += links[index].requests;
   }

   int blocksLeft = 0, piecesInHand = 0;
   for (int index = 0; index < workCount; index++) {
      if (work[index].piece < 0) continue;
      piecesInHand++;
      blocksLeft += work[index].left;
   }

   logTrace(TAG "pace: %d of %d pieces, %d in hand with %d blocks left, %d of %d peers %d talking %d serving, "
               "%d asked for, %d KB/s\n", torrent->piecesDone, torrent->meta.pieceCount, piecesInHand, blocksLeft,
           getLivePeerCount(), peerTarget, shookHands, serving, waiting, torrent->bytesPerSecond / 1024);
}

static void serviceDownloading(TorrentSlot *torrent)
{
   fillWorkSlots(torrent);
   if (!isAnyWorkBusy() && torrent->piecesDone >= torrent->meta.pieceCount) { finishTorrent(torrent); return; }

   servicePeers(torrent);
   dropPeersThatDoNotAnswer();
   rotatePeersThatKeepUsChoked();
   adjustPeerTarget(torrent);
   tickSpeed(torrent);
   reportPace(torrent);

   for (int index = 0; index < workCount; index++)
      if (work[index].piece >= 0 && work[index].left == 0) finishPiece(torrent, &work[index]);

   // section: nothing is arriving at all, so go back to the tracker for a fresh list of peers
   uint64_t quietFor = getTorrentNetwork()->getNowMs() - torrent->lastBlockAtMs;
   if (quietFor < PIECE_TIMEOUT_MS && (getLivePeerCount() > 0 || peerAttemptsLeft > 0)) return;

   logWarn(TAG "download: nothing has arrived for %ds, asking the trackers again\n", (int)(quietFor / 1000));
   closeAllPeers();
   waitAndAskAgain(torrent);
}

// What the engine can hold: pieces no larger than the buffer, and no more of them than the map of
// which pieces are in hand covers. A torrent past either is refused rather than stalling later.
static int canHoldTorrent(const TorrentMeta *meta)
{
   if (meta->pieceLength <= 0 || meta->pieceLength > pieceCapacity) {
      logError(TAG "engine: a torrent has pieces of %d KB, which this build cannot hold\n",
               meta->pieceLength / 1024);
      return 0;
   }

   if (meta->pieceCount > PIECE_BITMAP_MAX * 8) {
      logError(TAG "engine: a torrent is in %d pieces, more than the %d this build can keep track of\n",
               meta->pieceCount, PIECE_BITMAP_MAX * 8);
      return 0;
   }

   return 1;
}

// The description is in. It has to hash to the name the magnet gave, or it is not the torrent we
// asked for and nothing in it can be trusted.
static void takeDescription(TorrentSlot *torrent)
{
   uint8_t hash[SHA1_LENGTH];
   hashSha1(hash, torrent->description, torrent->descriptionSize);

   for (int index = 0; index < SHA1_LENGTH; index++) {
      if (hash[index] == torrent->meta.infoHash[index]) continue;

      logError(TAG "describe: what arrived is not the torrent the link named\n");
      failTorrent(torrent, "what arrived was not the torrent asked for");
      closeAllPeers();
      return;
   }

   // the trackers came from the link, and reading the description would wipe them
   char trackers[TRACKER_MAX][TRACKER_URL_MAX];
   int trackerCount = torrent->meta.trackerCount;
   for (int index = 0; index < trackerCount; index++)
      strCopy(trackers[index], TRACKER_URL_MAX, torrent->meta.trackers[index]);

   if (readTorrentInfo(torrent->description, torrent->descriptionSize, &torrent->meta) != 0 ||
       !canHoldTorrent(&torrent->meta) ||
       prepareTorrentStorage(&torrent->meta, downloads, torrent->contentPath, sizeof torrent->contentPath) != 0) {
      failTorrent(torrent, "its description could not be read");
      closeAllPeers();
      return;
   }

   for (int index = 0; index < trackerCount; index++)
      strCopy(torrent->meta.trackers[index], TRACKER_URL_MAX, trackers[index]);
   torrent->meta.trackerCount = trackerCount;

   logTrace(TAG "describe: %s, %d files, %lld bytes of content, %d pieces of %d KB\n", torrent->meta.name,
           torrent->meta.fileCount, (long long)torrent->meta.totalLength, torrent->meta.pieceCount,
           torrent->meta.pieceLength / 1024);

   torrent->secondStartedMs = getTorrentNetwork()->getNowMs();
   torrent->lastBlockAtMs = torrent->secondStartedMs;
   torrent->status = TORRENT_DOWNLOADING;
   planWork(torrent->meta.pieceLength);
}

static void serviceDescribing(TorrentSlot *torrent)
{
   servicePeers(torrent);
   if (torrent->status != TORRENT_DESCRIBING) return;   // a description too large for us fails here

   if (torrent->descriptionSize > 0 && torrent->descriptionLeft == 0) {
      takeDescription(torrent);
      return;
   }

   uint64_t spent = getTorrentNetwork()->getNowMs() - torrent->describingSinceMs;
   if (spent < DESCRIBE_TIMEOUT_MS && (getLivePeerCount() > 0 || peerAttemptsLeft > 0)) return;

   logError(TAG "describe: no peer would say what the torrent is\n");
   failTorrent(torrent, "no peer would say what it is");
   closeAllPeers();
}

// section: asking a tracker, one after another until one answers

static int startNextTracker(TorrentSlot *torrent)
{
   while (torrent->trackerIndex < torrent->meta.trackerCount) {
      const char *url = torrent->meta.trackers[torrent->trackerIndex++];

      // a magnet link does not say how large the torrent is, and a tracker that thinks we already
      // hold all of it may not bother naming peers
      int64_t left = torrent->meta.pieceCount > 0 ? torrent->meta.totalLength - torrent->bytesDone : (int64_t)1 << 40;

      if (startTrackerAnnounce(&torrent->announce, url, torrent->meta.infoHash, peerId, LISTEN_PORT, left) == 0)
         return 0;
   }

   return -1;
}

// A tracker that does not answer is usually busy rather than gone, a name that does not resolve is
// usually the tunnel still settling, and a swarm that serves nothing may serve later. All are worth
// another round before the torrent is given up on.
static void waitAndAskAgain(TorrentSlot *torrent)
{
   if (++torrent->trackerRounds > TRACKER_ROUNDS_MAX) {
      logWarn(TAG "download: giving up after %d rounds of asking\n", TRACKER_ROUNDS_MAX);
      failTorrent(torrent, "no tracker would say who has it");
      return;
   }

   logTrace(TAG "download: %s will ask the trackers again in %ds\n", torrent->meta.name, TRACKER_RETRY_MS / 1000);

   torrent->trackerIndex = 0;
   torrent->askAgainAtMs = getTorrentNetwork()->getNowMs() + TRACKER_RETRY_MS;
   torrent->status = TORRENT_WAITING;
}

static void serviceAsking(TorrentSlot *torrent)
{
   TrackerStatus status = serviceTrackerAnnounce(&torrent->announce);
   if (status == TRACKER_WORKING) return;

   if (status == TRACKER_ANSWERED && torrent->announce.reply.peerCount > 0) {
      torrent->peers = torrent->announce.reply;
      peerCursor = 0;
      peerAttemptsLeft = torrent->peers.peerCount * 3;   // a peer that is gone now may answer later
      torrent->secondStartedMs = getTorrentNetwork()->getNowMs();
      torrent->describingSinceMs = torrent->secondStartedMs;
      torrent->lastBlockAtMs = torrent->secondStartedMs;

      // a magnet link has no piece hashes yet, so the first thing to get is the description itself
      torrent->status = torrent->meta.pieceCount > 0 ? TORRENT_DOWNLOADING : TORRENT_DESCRIBING;
      if (torrent->status == TORRENT_DOWNLOADING) planWork(torrent->meta.pieceLength);
      return;
   }

   if (startNextTracker(torrent) != 0) waitAndAskAgain(torrent);
}

// section: what the app calls

int startTorrentEngine(const char *downloadsDirectory, uint8_t *pieceBuffer, int capacity)
{
   if (!getTorrentNetwork()) {
      logError(TAG "engine: no network was lent to the library\n");
      return -1;
   }

   if (makePeerId(peerId) != 0) {
      logError(TAG "engine: a peer id could not be made\n");
      return -1;
   }

   downloads = downloadsDirectory;
   piece = pieceBuffer;
   pieceCapacity = capacity;
   slotCount = 0;
   activeSlot = -1;

   closeAllPeers();
   return 0;
}

void stopTorrentEngine(void)
{
   closeAllPeers();

   for (int slot = 0; slot < slotCount; slot++) stopTrackerAnnounce(&slots[slot].announce);
   slotCount = 0;
   activeSlot = -1;
}

// the same torrent from two different results is still one torrent, and adding it twice would have
// both copies writing the same files
static int isAlreadyHeld(const uint8_t *infoHash)
{
   for (int slot = 0; slot < slotCount; slot++) {
      int same = 1;
      for (int index = 0; index < SHA1_LENGTH && same; index++) same = slots[slot].meta.infoHash[index] == infoHash[index];
      if (same) return 1;
   }

   return 0;
}

int addMagnet(const char *magnetUri, uint8_t *descriptionBuffer, int capacity)
{
   MagnetLink magnet;
   if (slotCount >= TORRENT_SLOT_MAX) return -1;

   if (readMagnetLink(&magnet, magnetUri) != 0) {
      logError(TAG "magnet: there is no torrent hash in that link\n");
      return -1;
   }

   if (magnet.trackerCount == 0) {
      logError(TAG "magnet: it names no tracker, and finding peers without one is not written yet\n");
      return -1;
   }

   if (isAlreadyHeld(magnet.infoHash)) {
      logInfo(TAG "add: that torrent is already in the list\n");
      return -1;
   }

   TorrentSlot *torrent = &slots[slotCount];
   memSet(torrent, 0, sizeof *torrent);
   memCopy(torrent->meta.infoHash, magnet.infoHash, SHA1_LENGTH);
   strCopy(torrent->meta.name, TORRENT_NAME_MAX, magnet.name);

   for (int index = 0; index < magnet.trackerCount; index++)
      strCopy(torrent->meta.trackers[index], TRACKER_URL_MAX, magnet.trackers[index]);
   torrent->meta.trackerCount = magnet.trackerCount;

   int mostWeCanHold = DESCRIPTION_PIECE_MAX * PEER_BLOCK_LENGTH;
   torrent->description = descriptionBuffer;
   torrent->descriptionCapacity = capacity < mostWeCanHold ? capacity : mostWeCanHold;
   torrent->status = TORRENT_WAITING;

   logTrace(TAG "magnet: %s, %d trackers\n", magnet.name, magnet.trackerCount);
   return slotCount++;
}

int addTorrent(const TorrentMeta *meta)
{
   if (slotCount >= TORRENT_SLOT_MAX) return -1;
   if (!canHoldTorrent(meta)) return -1;

   if (isAlreadyHeld(meta->infoHash)) {
      logInfo(TAG "add: that torrent is already in the list\n");
      return -1;
   }

   TorrentSlot *torrent = &slots[slotCount];
   memSet(torrent, 0, sizeof *torrent);
   torrent->meta = *meta;
   torrent->status = TORRENT_WAITING;

   return slotCount++;
}

void serviceTorrentEngine(void)
{
   // section: whose turn it is
   if (activeSlot >= 0 && slots[activeSlot].status != TORRENT_ASKING &&
       slots[activeSlot].status != TORRENT_DESCRIBING && slots[activeSlot].status != TORRENT_DOWNLOADING) {
      freeAllWork(&slots[activeSlot]);   // its turn is over, so what it had in hand goes back on offer
      activeSlot = -1;
   }

   if (activeSlot < 0) {
      for (int slot = 0; slot < slotCount && activeSlot < 0; slot++) {
         if (slots[slot].status != TORRENT_WAITING) continue;
         if (getTorrentNetwork()->getNowMs() < slots[slot].askAgainAtMs) continue;

         // a magnet link cannot say where its content goes until the description has arrived
         TorrentSlot *torrent = &slots[slot];
         int ready = torrent->meta.pieceCount == 0 ||
                     prepareTorrentStorage(&torrent->meta, downloads, torrent->contentPath,
                                           sizeof torrent->contentPath) == 0;

         if (!ready) {
            failTorrent(torrent, "there is nowhere on disk to put it");
            continue;
         }

         // no tracker would even be asked: usually a name that will resolve once the tunnel settles
         if (startNextTracker(torrent) != 0) {
            waitAndAskAgain(torrent);
            continue;
         }

         torrent->status = TORRENT_ASKING;
         activeSlot = slot;
         peerTarget = PEER_START_COUNT;   // this swarm may be a better one to ask than the last
         freeAllWork(NULL);   // the buffer is laid out for whichever torrent is downloading now
      }
   }

   if (activeSlot < 0) return;

   // section: a slice of work for it
   TorrentSlot *torrent = &slots[activeSlot];
   if (torrent->status == TORRENT_ASKING) serviceAsking(torrent);
   else if (torrent->status == TORRENT_DESCRIBING) serviceDescribing(torrent);
   else if (torrent->status == TORRENT_DOWNLOADING) serviceDownloading(torrent);
}

int getTorrentCount(void)
{
   return slotCount;
}

void getTorrentProgress(int slot, TorrentProgress *progress)
{
   memSet(progress, 0, sizeof *progress);
   if (slot < 0 || slot >= slotCount) return;

   const TorrentSlot *torrent = &slots[slot];
   progress->status = torrent->status;
   progress->name = torrent->meta.name;
   progress->piecesDone = torrent->piecesDone;
   progress->pieceCount = torrent->meta.pieceCount;
   progress->bytesDone = torrent->bytesDone;
   progress->totalLength = torrent->meta.totalLength;
   progress->peerCount = torrent->peers.peerCount;
   progress->checking = torrent->resuming && torrent->status == TORRENT_DOWNLOADING;
   progress->seederCount = torrent->peers.seeders;
   progress->connectedCount = slot == activeSlot ? getLivePeerCount() : 0;
   progress->bytesPerSecond = torrent->status == TORRENT_DOWNLOADING ? torrent->bytesPerSecond : 0;
   progress->reason = torrent->reason;
}

void pauseTorrent(int slot)
{
   if (slot < 0 || slot >= slotCount) return;

   if (slot == activeSlot) {
      closeAllPeers();
      freeAllWork(&slots[slot]);
      stopTrackerAnnounce(&slots[slot].announce);   // or its port stays open until the app closes
      activeSlot = -1;
   }

   slots[slot].status = TORRENT_PAUSED;
}

void resumeTorrent(int slot)
{
   if (slot < 0 || slot >= slotCount || slots[slot].status != TORRENT_PAUSED) return;

   slots[slot].trackerIndex = 0;
   slots[slot].trackerRounds = 0;
   slots[slot].askAgainAtMs = 0;
   slots[slot].status = TORRENT_WAITING;
}

void removeTorrent(int slot)
{
   if (slot < 0 || slot >= slotCount) return;

   if (slot == activeSlot) {
      closeAllPeers();
      freeAllWork(&slots[slot]);
      stopTrackerAnnounce(&slots[slot].announce);
      activeSlot = -1;
   }

   for (int index = slot; index + 1 < slotCount; index++) slots[index] = slots[index + 1];
   slotCount--;

   if (activeSlot > slot) activeSlot--;
}

int getTorrentContentPath(int slot, char *out, int capacity)
{
   if (slot < 0 || slot >= slotCount) return -1;

   const TorrentSlot *torrent = &slots[slot];
   if (torrent->contentPath[0] == 0) return -1;   // a magnet whose description has not arrived has no path yet

   if (torrent->meta.fileCount != 1) {
      strCopy(out, capacity, torrent->contentPath);
      return 1;
   }

   joinPath(out, capacity, torrent->contentPath, torrent->meta.files[0].path);
   return 0;
}

