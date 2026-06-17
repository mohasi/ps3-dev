#pragma once

#include <stdint.h>

// Minimal reader for the single-file .rpk game bundle produced by renpy-to-ps3.
// Layout (all integers little-endian; we assemble bytes explicitly because the
// PS3 PPU is big-endian):
//   magic "RPK1" | u32 version | u32 entryCount
//   TOC: entryCount * { u32 nameLen; name(utf8); u64 offset; u64 length }
//   blobs.

typedef struct {
   int      fd;
   int      open;
   uint32_t version;
   uint32_t count;
} RpkFile;

// Opens and validates the .rpk header. Returns 0 on success, negative on error.
int  openRpk(RpkFile *r, const char *path);
void closeRpk(RpkFile *r);

// Walks the TOC. Copies the "manifest" entry text into manifestOut (cap bytes,
// NUL-terminated) and reports the byte length of "game.rbc" via *gameRbcLen
// (-1 if absent). Returns 0 on success, negative on error.
int  readRpkInfo(RpkFile *r, char *manifestOut, int cap, long *gameRbcLen);

// Reads a whole entry by name into a freshly malloc'd buffer (caller frees with free()).
// *outBuf / *outLen receive the buffer and its length. Returns 0 on success, negative on error.
int  readRpkEntry(RpkFile *r, const char *name, unsigned char **outBuf, long *outLen);

// Reads the index-th (0-based) entry whose name ends with suffix (case-insensitive,
// e.g. ".ttf"). Copies the matched name into outName (outNameCap). Buffer caller-freed.
// Returns 0 on success, negative if there is no such match.
int  readRpkEntrySuffix(RpkFile *r, const char *suffix, int index, char *outName, int outNameCap,
                        unsigned char **outBuf, long *outLen);
