#pragma once
#include <stdint.h>
#include "amg-response.h"

// gnudb / CDDB lookup helpers. The disc-id computation and the record parser are pure and host-testable;
// the actual HTTP GETs are done by the caller (plain HTTP to gnudb.gnudb.org, no TLS).

// Standard freedb/CDDB disc id from the drive TOC. frameOffsets[i] = start frame (LBA + 150) of track i;
// leadoutFrame = start frame of the lead-out. Returns the 32-bit id (low byte = track count).
uint32_t computeCddbDiscId(const uint32_t *frameOffsets, int nTracks, uint32_t leadoutFrame);

// Format the CDDB "query" URL path+args for the given TOC into out. Returns length or -1.
int buildCddbQueryUrl(char *out, int outCap, uint32_t discId, const uint32_t *frameOffsets, int nTracks, uint32_t leadoutFrame);

// Parse a CDDB "query" reply and pick the first match's category + disc id (needed for the follow-up
// "read"). Handles 200 (one exact), 210/211 (list; takes the first line). Copies into catOut/idOut,
// NUL-terminated. Returns 0 on a match, -1 if none (202) or malformed.
int parseCddbQuery(const char *reply, char *catOut, int catCap, char *idOut, int idCap);

// Parse a CDDB "read" record (the xmcd text) IN PLACE: NUL-terminates the fields inside text and points
// album->albumTitle/albumArtist and trackBuf[i].title into it. album->tracks is set to trackBuf.
// Returns the track count found (0 if the record carries no titles).
int parseCddbRecord(char *text, AmgAlbum *album, AmgTrack *trackBuf, int maxTracks);
