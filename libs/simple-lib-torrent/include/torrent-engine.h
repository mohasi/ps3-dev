#pragma once

// The downloads, as something the app keeps rather than something it waits for.
//
// An app adds torrents, then calls serviceTorrentEngine every time round its own loop. Each call
// does a slice of work and returns, so a screen stays responsive and progress can be read at any
// moment. One torrent downloads at a time and the rest wait their turn, which keeps the memory to a
// single piece.

#include <stdint.h>

#include "torrent-file.h"
#include "tracker.h"

#define TORRENT_SLOT_MAX 8   // torrents the engine holds at once

typedef enum {
   TORRENT_WAITING,       // added, its turn has not come
   TORRENT_ASKING,        // asking a tracker who has it
   TORRENT_DESCRIBING,    // from a magnet link: asking a peer what the torrent actually is
   TORRENT_DOWNLOADING,
   TORRENT_PAUSED,
   TORRENT_FINISHED,
   TORRENT_FAILED
} TorrentStatus;

#define TORRENT_REASON_MAX 48   // why it failed, in words a screen can show

typedef struct {
   TorrentStatus status;
   const char   *name;
   const char   *reason;            // why it failed, empty otherwise
   int           piecesDone;
   int           pieceCount;
   int64_t       bytesDone;
   int64_t       totalLength;
   int           peerCount;         // peers the tracker named
   int           checking;          // reading back what an earlier run left on disk, not downloading yet
   int           seederCount;       // of those, the ones said to hold all of it
   int           connectedCount;    // peers we are actually talking to now
   int           bytesPerSecond;    // over the last few seconds, 0 when nothing is moving
} TorrentProgress;

// Set the engine up. downloadsDirectory is where content goes, pieceBuffer holds one piece and has
// to be as large as the largest piece of any torrent added. Returns 0, or -1.
int startTorrentEngine(const char *downloadsDirectory, uint8_t *pieceBuffer, int pieceCapacity);
void stopTorrentEngine(void);

// Take on a torrent. The description is copied, but its piece hashes still point into the document
// it was read from, so that document has to stay put. Returns the slot it went into, or -1 when
// there is no room.
int addTorrent(const TorrentMeta *meta);

// Take on a magnet link. It says only the hash, a name and some trackers, so the description is
// asked of the first peer that will serve it and assembled into descriptionBuffer, which has to stay
// put for as long as the torrent does. A megabyte covers all but the largest torrents. Returns the
// slot, or -1.
int addMagnet(const char *magnetUri, uint8_t *descriptionBuffer, int capacity);

// Do a slice of work, and return. Nothing here waits, so call it as often as the app likes.
void serviceTorrentEngine(void);

int  getTorrentCount(void);
void getTorrentProgress(int slot, TorrentProgress *progress);
void pauseTorrent(int slot);
void resumeTorrent(int slot);

// Take one off the list. What is on disk is left where it is, so a torrent removed by mistake costs
// nothing but the asking again.
void removeTorrent(int slot);

// Where this torrent's content sits, for an app that wants to delete it. Returns 1 when the path is
// a folder holding the whole torrent, 0 when it is the single file, or -1 when there is no slot.
int getTorrentContentPath(int slot, char *out, int capacity);
