#pragma once
#include <stdint.h>

// Renders an AMG "get track info" response body from plain album/track metadata (as fetched from an
// online source). The byte format is the one the firmware's x3_amgsdk decoder expects; pure byte
// building with no platform dependencies, so it also compiles + runs on the host for verification.

typedef struct {
   const char *title;      // track title as shown in the XMB music column
} AmgTrack;

typedef struct {
   const char     *albumTitle;
   const char     *albumArtist;
   const char     *albumGenre;   // often absent from a CDDB record; empty then
   const AmgTrack *tracks;
   int             trackCount;
} AmgAlbum;

// Build the response body (the 'A' album record: tag + 4-byte little-endian length + album container)
// into out[0..outCap). scratch[0..scratchCap) is working space for the nested containers (needs to be
// a few KB larger than the output). Returns the body length, or -1 if anything does not fit.
int buildAmgResponse(const AmgAlbum *album, unsigned char *scratch, int scratchCap, unsigned char *out, int outCap);
