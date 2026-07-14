#pragma once

// text-sanitize - cleaning externally-sourced text (subtitles, feeds, scraped html/xml) for display.
// libc-free (prx-safe), everything in place, like the rest of simple-lib-core.

#include "string-utilities.h"   // startsWith

// decode &amp; &lt; &gt; &quot; &apos; and numeric &#N; / &#xN; in place. numeric code points are
// re-encoded as UTF-8 (up to 3 bytes; astral code points and unknown entities are dropped). one
// pass; returns 1 if anything changed, so a caller can repeat it for double-escaped text.
static inline int decodeXmlEntities(char *text)
{
   int changed = 0, j = 0;
   for (int i = 0; text[i];) {
      if (text[i] != '&') { text[j++] = text[i++]; continue; }

      int semi = -1;   // entity terminator within a sane distance
      for (int k = 1; k <= 8 && text[i + k]; k++) if (text[i + k] == ';') { semi = i + k; break; }
      if (semi < 0) { text[j++] = text[i++]; continue; }

      unsigned code = 0;
      if      (startsWith(text + i, "&amp;"))  code = '&';
      else if (startsWith(text + i, "&lt;"))   code = '<';
      else if (startsWith(text + i, "&gt;"))   code = '>';
      else if (startsWith(text + i, "&quot;")) code = '"';
      else if (startsWith(text + i, "&apos;")) code = '\'';
      else if (text[i + 1] == '#') {
         int k = i + 2, hex = (text[k] == 'x' || text[k] == 'X');
         if (hex) k++;
         for (; k < semi; k++) {
            char c = text[k];
            int digit = (c >= '0' && c <= '9') ? c - '0'
                      : (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (digit < 0) { code = 0; break; }
            code = code * (hex ? 16u : 10u) + (unsigned)digit;
         }
      } else { text[j++] = text[i++]; continue; }

      // utf-8 encode the code point (0 = unrecognised entity, dropped)
      if      (code == 0)      {}
      else if (code < 0x80)    text[j++] = (char)code;
      else if (code < 0x800)   { text[j++] = (char)(0xC0 | (code >> 6));  text[j++] = (char)(0x80 | (code & 0x3F)); }
      else if (code < 0x10000) { text[j++] = (char)(0xE0 | (code >> 12)); text[j++] = (char)(0x80 | ((code >> 6) & 0x3F)); text[j++] = (char)(0x80 | (code & 0x3F)); }
      i = semi + 1;
      changed = 1;
   }
   text[j] = 0;
   return changed;
}

// drop <b>/<i>/<font ...> style markup tags - plain-text rendering would show them as code.
// a lone '<' with no closing '>' (e.g. "5 < 10") is kept as text.
static inline void stripMarkupTags(char *text)
{
   int j = 0;
   for (int i = 0; text[i];) {
      if (text[i] == '<') {
         int close = -1;
         for (int k = i + 1; text[k]; k++) if (text[k] == '>') { close = k; break; }
         if (close >= 0) { i = close + 1; continue; }
      }
      text[j++] = text[i++];
   }
   text[j] = 0;
}

// remove invisible layout code points fonts draw as boxes or stray gaps: zero-width marks
// (U+200B-U+200F), directional marks (U+202A-U+202E), word joiner (U+2060) and BOM (U+FEFF);
// a non-breaking space (U+00A0) becomes a plain space. matched as raw UTF-8 byte sequences.
static inline void removeInvisibleMarks(char *text)
{
   unsigned char *bytes = (unsigned char *)text;
   int j = 0;
   for (int i = 0; bytes[i];) {
      if (bytes[i] == 0xC2 && bytes[i + 1] == 0xA0) { bytes[j++] = ' '; i += 2; continue; }
      if (bytes[i] == 0xE2 && bytes[i + 1] == 0x80 && ((bytes[i + 2] >= 0x8B && bytes[i + 2] <= 0x8F) || (bytes[i + 2] >= 0xAA && bytes[i + 2] <= 0xAE))) { i += 3; continue; }
      if (bytes[i] == 0xE2 && bytes[i + 1] == 0x81 && bytes[i + 2] == 0xA0) { i += 3; continue; }
      if (bytes[i] == 0xEF && bytes[i + 1] == 0xBB && bytes[i + 2] == 0xBF) { i += 3; continue; }
      bytes[j++] = bytes[i++];
   }
   bytes[j] = 0;
}

// newlines/tabs become spaces, runs of spaces collapse to one, leading/trailing spaces go.
static inline void flattenWhitespace(char *text)
{
   int j = 0;
   for (int i = 0; text[i]; i++) {
      char ch = (text[i] == '\n' || text[i] == '\r' || text[i] == '\t') ? ' ' : text[i];
      if (ch == ' ' && (j == 0 || text[j - 1] == ' ')) continue;
      text[j++] = ch;
   }
   while (j > 0 && text[j - 1] == ' ') j--;
   text[j] = 0;
}
