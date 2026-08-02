#include "amg-response.h"
#include "string-utilities.h"   // memCopy/memSet/getStrLen - PRX-friendly (no libc memcpy/memset, which won't resolve at load)

// Everything is built bottom-up into a caller-supplied scratch arena as (pointer, length) fields; a
// container copies its child fields' bytes into a fresh arena block. A single failed flag on the arena
// turns any overflow into a clean -1 at the end rather than a half-built response.

#define MAX_TRACKS 99   // a Red Book audio CD holds at most 99 tracks

// Which slot drives which XMB label was mapped on a real console by giving every candidate slot its own
// marker string and reading them back off the screen:
//    album 1 -> "Album"   album 4 -> "Artist" (disc icon)   album 9 -> "Genre"   album 11 -> discs in set
//    track 1 -> "Track"   track 6, string 2 -> "Artist" (track screens)
// Every other slot displays nothing. Two things to know before changing any of this. A slot sent as an
// empty string leaves whatever the XMB already had on screen, so a blank can look like an old value
// lingering rather than a slot we never filled. And the slots left completely empty (10, 13, 14, 16, 17,
// 18, 21, 24, 25) hard-lock the console if a string is written into them -- two separate attempts with
// different slots from that set both locked, so they are not text slots at all.

typedef struct { const unsigned char *data; int length; } Field;
typedef struct { unsigned char *buf; int cap, used, failed; } Arena;

static unsigned char *reserveArena(Arena *a, int n)
{
   if (a->failed || a->used + n > a->cap) { a->failed = 1; return 0; }
   unsigned char *p = a->buf + a->used;
   a->used += n;
   return p;
}

static void putLe16(unsigned char *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void putLe32(unsigned char *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

static Field emitEmpty(void) { Field f = { 0, 0 }; return f; }

// ASCII string field, NUL-terminated (the decoder copies length-1 bytes; the terminator must be present).
static Field emitStr(Arena *a, const char *s)
{
   int n = getStrLen(s) + 1;
   unsigned char *p = reserveArena(a, n);
   if (!p) return emitEmpty();
   memCopy(p, s, n);
   Field f = { p, n };
   return f;
}

static Field emitI16(Arena *a, uint16_t v)
{
   unsigned char *p = reserveArena(a, 2);
   if (!p) return emitEmpty();
   putLe16(p, v);
   Field f = { p, 2 };
   return f;
}

static Field emitI32(Arena *a, uint32_t v)
{
   unsigned char *p = reserveArena(a, 4);
   if (!p) return emitEmpty();
   putLe32(p, v);
   Field f = { p, 4 };
   return f;
}

// container = [1-byte field count N][ (N+1) little-endian u32 offsets ][ field data ];
// field i occupies bytes [offset[i] .. offset[i+1]) measured from the container start.
static Field emitContainer(Arena *a, const Field *fields, int n)
{
   int header = 1 + (n + 1) * 4;
   int dataLen = 0;
   for (int i = 0; i < n; i++) dataLen += fields[i].length;
   int total = header + dataLen;

   unsigned char *out = reserveArena(a, total);
   if (!out) return emitEmpty();

   out[0] = (unsigned char)n;
   int off = header;
   putLe32(out + 1, (uint32_t)off);                                  // offset[0]
   for (int i = 0; i < n; i++) { off += fields[i].length; putLe32(out + 1 + (i + 1) * 4, (uint32_t)off); }

   int w = header;
   for (int i = 0; i < n; i++) { memCopy(out + w, fields[i].data, fields[i].length); w += fields[i].length; }

   Field f = { out, total };
   return f;
}

// one 17-field track record. field 1 is the track title, shown as "Track". field 6 is a container the
// decoder at 0xa0c0 reads as 4 sub-strings, and they are four different things rather than four copies:
// the second is this track's own artist, which is what the XMB shows as "Artist" whenever the album-level
// artist is absent. The first holds the title, as it always has; the purpose of the last two is unknown,
// so they are sent empty. Fields 15 and 16 MUST be present as int16s or the track decoder at 0x5fc4
// rejects the whole album -> they carry no display data, so they are zero.
static Field emitTrack(Arena *a, const char *title, const char *artist)
{
   Field parts[4] = { emitStr(a, title), emitStr(a, artist), emitStr(a, ""), emitStr(a, "") };
   Field partsObject = emitContainer(a, parts, 4);
   Field partsList   = emitContainer(a, &partsObject, 1);

   Field fields[17];
   memSet(fields, 0, sizeof fields);   // an all-zero Field is emitEmpty()
   fields[1]  = emitStr(a, title);
   fields[6]  = partsList;
   fields[15] = emitI16(a, 0);
   fields[16] = emitI16(a, 0);
   return emitContainer(a, fields, 17);
}

int buildAmgResponse(const AmgAlbum *album, unsigned char *scratch, int scratchCap, unsigned char *out, int outCap)
{
   Arena a = { scratch, scratchCap, 0, 0 };

   int trackCount = album->trackCount;
   if (trackCount > MAX_TRACKS) trackCount = MAX_TRACKS;

   const char *albumArtist = album->albumArtist ? album->albumArtist : "";
   const char *albumTitle  = album->albumTitle ? album->albumTitle : "";
   const char *albumGenre  = album->albumGenre ? album->albumGenre : "";

   // field 15 = one group record wrapping the track array plus two numbers. The first is this disc's
   // number within a set, shown as the first half of "Disc Number" on the XMB; an audio CD is always a
   // single disc, so it is 1. The second is not displayed anywhere we have found, and neither of them
   // sizes the track list -- the list carries its own count -- so the track count here was only ever
   // making the XMB report the disc as "30 of 30".
   Field trackFields[MAX_TRACKS];
   for (int i = 0; i < trackCount; i++) trackFields[i] = emitTrack(&a, album->tracks[i].title, albumArtist);
   Field trackList   = emitContainer(&a, trackFields, trackCount);
   Field groupFields[3] = { trackList, emitI16(&a, 1), emitI16(&a, (uint16_t)trackCount) };
   Field group       = emitContainer(&a, groupFields, 3);
   Field field15     = emitContainer(&a, &group, 1);

   // album = 30-field container. present-but-empty string slots carry a lone NUL; unused list/array
   // slots are truly empty; the int16 slots (28/29) are structural, and 29 > 6 selects the track view.
   // The slots the XMB displays are 1 (Album), 4 (Artist), 9 (Genre) and 11 (discs in the set). Slot 0
   // acts as a switch rather than a value: its own contents never appear, but filling it makes the track
   // screens take the artist from slot 4 for the whole disc, and leaving it empty makes them take each
   // track's own artist. We leave it empty so a compilation shows the right artist per track. Slot 4 is
   // still read by the disc's own icon on the XMB -- both levels are live at once.
   Field af[30];
   memSet(af, 0, sizeof af);   // an all-zero Field is emitEmpty()
   static const int nulSlots[] = { 0, 2, 3, 5, 8, 12, 19, 20, 22, 26, 27 };
   for (unsigned k = 0; k < sizeof nulSlots / sizeof nulSlots[0]; k++) af[nulSlots[k]] = emitStr(&a, "");
   af[1]  = emitStr(&a, albumTitle);
   af[4]  = emitStr(&a, albumArtist);
   af[6]  = emitI16(&a, (uint16_t)trackCount);
   af[7]  = emitI16(&a, 1);
   af[9]  = emitStr(&a, albumGenre);
   af[11] = emitI16(&a, 1);   // how many discs in the set: the second half of "Disc Number"
   af[15] = field15;
   af[23] = emitI32(&a, 1);
   af[28] = emitI16(&a, 0);
   af[29] = emitI16(&a, 7);
   Field albumContainer = emitContainer(&a, af, 30);

   if (a.failed) return -1;

   int body = 5 + albumContainer.length;
   if (body > outCap) return -1;
   out[0] = 'A';
   putLe32(out + 1, (uint32_t)albumContainer.length);
   memCopy(out + 5, albumContainer.data, albumContainer.length);
   return body;
}
