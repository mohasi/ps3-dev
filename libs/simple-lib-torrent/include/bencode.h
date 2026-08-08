#pragma once

// Bencode, the format a .torrent file and a tracker's reply are written in. Four kinds of value:
//
//   i-42e            an integer
//   4:spam           a string, its length first
//   l...e            a list
//   d...e            a dictionary, keys are strings and come in order
//
// Nothing is copied and nothing is allocated: a value is an offset and a length into the document
// the caller already holds. That matters for one thing in particular, the info hash, which is the
// SHA-1 of the exact bytes of the "info" dictionary as they arrived, not of anything re-encoded.

#include <stdint.h>

typedef enum {
   BENCODE_INTEGER,
   BENCODE_STRING,
   BENCODE_LIST,
   BENCODE_DICTIONARY
} BencodeKind;

typedef struct {
   BencodeKind kind;
   int         start;   // first byte of the whole value
   int         end;     // one past its last byte
   int         innerStart;   // for a string, its text; for a list or dictionary, its first member
   int         innerEnd;
} BencodeValue;

// Read the value that starts at offset. Returns the offset just past it, or -1 when the document is
// malformed there. Depth is bounded, so a document made of nothing but nested lists cannot run away.
int readBencode(const uint8_t *document, int length, int offset, BencodeValue *out);

// Find a member of a dictionary by name. 0 when it is there, -1 when it is not.
int findBencodeMember(const uint8_t *document, int length, const BencodeValue *dictionary, const char *name,
                      BencodeValue *out);

// Read the next value of a list or dictionary, starting at offset (innerStart for the first). Returns
// the offset past it, 0 once the end has been reached, or -1 when it is malformed.
int readBencodeItem(const uint8_t *document, int length, const BencodeValue *container, int offset,
                    BencodeValue *out);

int64_t getBencodeInteger(const uint8_t *document, const BencodeValue *value);   // 0 when it is not one

// point text at a string's bytes, which are not terminated. returns its length, or -1.
int getBencodeString(const uint8_t *document, const BencodeValue *value, const uint8_t **text);

// copy a string value out as ordinary text, truncated to fit. 0 / -1.
int copyBencodeString(const uint8_t *document, const BencodeValue *value, char *out, int capacity);
