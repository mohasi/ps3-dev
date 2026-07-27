#include "cddb.h"

static int sumDecimalDigits(int n) { int s = 0; while (n > 0) { s += n % 10; n /= 10; } return s; }

uint32_t computeCddbDiscId(const uint32_t *frameOffsets, int nTracks, uint32_t leadoutFrame)
{
   if (nTracks < 1) return 0;
   int checksum = 0;
   for (int i = 0; i < nTracks; i++) checksum += sumDecimalDigits((int)(frameOffsets[i] / 75));   // 75 frames per second
   int totalSeconds = (int)(leadoutFrame / 75) - (int)(frameOffsets[0] / 75);
   return ((uint32_t)(checksum % 255) << 24) | ((uint32_t)totalSeconds << 8) | (uint32_t)nTracks;
}

// tiny helpers so the module stays free of libc string funcs (matches the plugin's no-libc rule)
static int appendStr(char *out, int cap, int at, const char *s) { int i = 0; while (s[i] && at < cap - 1) out[at++] = s[i++]; return at; }
static int appendUint(char *out, int cap, int at, uint32_t v) {
   char tmp[12]; int n = 0;
   if (v == 0) tmp[n++] = '0';
   while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
   while (n > 0 && at < cap - 1) out[at++] = tmp[--n];
   return at;
}
static int appendHex8(char *out, int cap, int at, uint32_t v) {
   for (int shift = 28; shift >= 0 && at < cap - 1; shift -= 4) out[at++] = "0123456789abcdef"[(v >> shift) & 0xF];
   return at;
}

int buildCddbQueryUrl(char *out, int outCap, uint32_t discId, const uint32_t *frameOffsets, int nTracks, uint32_t leadoutFrame)
{
   int at = 0;
   at = appendStr(out, outCap, at, "/~cddb/cddb.cgi?cmd=cddb+query+");
   at = appendHex8(out, outCap, at, discId);
   at = appendStr(out, outCap, at, "+");
   at = appendUint(out, outCap, at, (uint32_t)nTracks);
   for (int i = 0; i < nTracks; i++) { at = appendStr(out, outCap, at, "+"); at = appendUint(out, outCap, at, frameOffsets[i]); }
   at = appendStr(out, outCap, at, "+");
   at = appendUint(out, outCap, at, leadoutFrame / 75);                    // total disc length in seconds
   at = appendStr(out, outCap, at, "&hello=user+ps3+abcde+2.9.3&proto=6"); // gnudb rejects unknown client names; this one is accepted
   if (at >= outCap - 1) return -1;
   out[at] = '\0';
   return at;
}

static int startsWith(const char *s, const char *prefix) { int i = 0; while (prefix[i]) { if (s[i] != prefix[i]) return 0; i++; } return 1; }

// copy one space-delimited token starting at *p into out (NUL-terminated), advance *p past it and any
// following spaces. returns the token length.
static int takeToken(const char **p, char *out, int cap) {
   const char *s = *p;
   int n = 0;
   while (*s == ' ') s++;
   while (*s && *s != ' ' && *s != '\r' && *s != '\n') { if (n < cap - 1) out[n++] = *s; s++; }
   if (cap > 0) out[n] = '\0';
   while (*s == ' ') s++;
   *p = s;
   return n;
}

int parseCddbQuery(const char *reply, char *catOut, int catCap, char *idOut, int idCap)
{
   if (catCap > 0) catOut[0] = '\0';
   if (idCap > 0) idOut[0] = '\0';
   if (!reply || (reply[0] != '2')) return -1;

   // "200 <cat> <id> <artist / title>"  -> code then cat, id on the same line
   // "210"/"211 ..." (list) -> the match lines start on the NEXT line: "<cat> <id> <artist / title>"
   char code[8];
   const char *p = reply;
   if (takeToken(&p, code, sizeof code) < 3) return -1;   // need a full 3-digit status
   const char *matchLine;
   if (code[0] == '2' && code[1] == '0' && code[2] == '0') {
      matchLine = p;                                  // rest of the first line
   } else if (code[2] == '0' || code[2] == '1') {     // 210 / 211: skip to the next line
      const char *nl = p; while (*nl && *nl != '\n') nl++; while (*nl == '\n' || *nl == '\r') nl++;
      matchLine = nl;
   } else {
      return -1;                                      // 202 no match, 403 db entry corrupt, etc.
   }
   int c = takeToken(&matchLine, catOut, catCap);
   int i = takeToken(&matchLine, idOut, idCap);
   return (c > 0 && i > 0) ? 0 : -1;
}

// Walk the record one line at a time, NUL-terminating each line in place. DTITLE gives "Artist / Album";
// TTITLE<n> gives track n's title. CDDB lets a value continue over repeated same-key lines; we take the
// first line of each key and do not concatenate continuations, so a title split across lines is truncated
// to its first part (rare for the discs this targets).
int parseCddbRecord(char *text, AmgAlbum *album, AmgTrack *trackBuf, int maxTracks)
{
   album->albumArtist = "";
   album->albumTitle  = "";
   for (int i = 0; i < maxTracks; i++) trackBuf[i].title = "";
   int highestTrack = -1;

   char *p = text;
   while (*p) {
      char *line = p;
      while (*p && *p != '\n' && *p != '\r') p++;
      char *term = p;
      while (*p == '\n' || *p == '\r') p++;   // p -> next line start
      *term = '\0';                            // terminate this line

      if (startsWith(line, "DTITLE=") && album->albumTitle[0] == '\0') {   // first DTITLE wins
         char *value = line + 7;
         char *sep = 0;
         for (char *q = value; q[0] && q[1] && q[2]; q++)
            if (q[0] == ' ' && q[1] == '/' && q[2] == ' ') { sep = q; break; }
         if (sep) { sep[0] = '\0'; album->albumArtist = value; album->albumTitle = sep + 3; }
         else     { album->albumTitle = value; }
      } else if (startsWith(line, "TTITLE")) {
         char *q = line + 6;
         int index = 0, sawDigit = 0;
         while (*q >= '0' && *q <= '9') { index = index < maxTracks ? index * 10 + (*q - '0') : maxTracks; q++; sawDigit = 1; }
         if (sawDigit && *q == '=' && index < maxTracks && trackBuf[index].title[0] == '\0') {   // first TTITLEn wins
            trackBuf[index].title = q + 1;
            if (index > highestTrack) highestTrack = index;
         }
      }
   }

   album->tracks = trackBuf;
   album->trackCount = highestTrack + 1;
   return album->trackCount;
}
