#pragma once

// Do a live gnudb lookup for the currently inserted disc and build the AMG HTTP response (headers + body)
// into out[0..outCap). Returns the total response length, or <=0 on any failure (drive read, network,
// no match, build overflow) so the caller can fall back to the static file. Reads the disc TOC, computes
// the CDDB disc id, GETs the gnudb query + read over plain HTTP, and renders the album. Uses one
// on-demand 64 KB heap page that is freed before returning, so nothing stays resident.
int buildLiveResponse(char *out, int outCap);
