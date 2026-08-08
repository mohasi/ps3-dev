#pragma once

// Where a torrent's content lands. A torrent is one long run of bytes cut into pieces, and the files
// are laid end to end along it, so a piece can start in one file and finish in the next.
//
// While it is downloading everything sits under an "incomplete" folder, with the same shape it will
// have when it is done. Nothing appears in the downloads folder itself until every piece is in.

#include <stdint.h>

#include "torrent-file.h"

// The folder this torrent is being built in, below downloadsDirectory. A torrent of one file writes
// that file straight into the folder; one of several gets a folder of its own. Creates what it
// needs. Returns 0, or -1.
int prepareTorrentStorage(const TorrentMeta *meta, const char *downloadsDirectory, char *contentPath, int capacity);

// write one piece where it belongs, across as many files as it spans. 0 / -1.
int writeTorrentPiece(const TorrentMeta *meta, const char *contentPath, int pieceIndex, const uint8_t *data,
                      int length);

// Read a piece back and check it against its hash, which is how a download that was interrupted
// knows what it already has. 1 when the piece is there and good, 0 when it is not.
int isPieceOnDisk(const TorrentMeta *meta, const char *contentPath, int pieceIndex, uint8_t *scratch, int capacity);

// Move a finished torrent out of the incomplete folder. Does nothing when contentPath says it was
// downloaded straight into place, which is what happens when an earlier run already finished it.
// 0 / -1.
int finishTorrentStorage(const TorrentMeta *meta, const char *downloadsDirectory, const char *contentPath);
