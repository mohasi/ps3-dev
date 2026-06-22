#pragma once

#include <stdint.h>
#include <cell/rtc.h>

// utf-8 em dash (U+2014). used in ui as a "value not known yet" placeholder
// (e.g. folder size while the background sizer is still walking).
#define EM_DASH "\xe2\x80\x94"

// Returns length of a string.
static inline int getStrLen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

// Byte-wise copy. Use instead of libc memcpy in code that links against a
// stripped libc (e.g. vsh prx imports — memcpy is not exported there).
static inline void memCopy(void *dst, const void *src, int n)
{
   unsigned char *d = (unsigned char *)dst;
   const unsigned char *s = (const unsigned char *)src;
   for (int i = 0; i < n; i++) d[i] = s[i];
}

// Byte-wise fill. Use instead of libc memset in prx-sensitive code.
static inline void memSet(void *dst, unsigned char value, int n)
{
   unsigned char *d = (unsigned char *)dst;
   for (int i = 0; i < n; i++) d[i] = value;
}

// Strict string equality. Same reason as memCopy: avoids libc strcmp.
static inline int strEq(const char *a, const char *b)
{
   while (*a && *b && *a == *b) { a++; b++; }
   return *a == *b;
}

// Returns 1 if s starts with prefix, 0 otherwise.
static inline int startsWith(const char *s, const char *prefix)
{
   while (*prefix && *s && *s == *prefix) { s++; prefix++; }
   return *prefix == '\0';
}

// ASCII-only uppercase for a single character.
static inline char toUpperChar(char c)
{
   return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

// ASCII-only lowercase for a single character.
static inline char toLowerChar(char c)
{
   return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Normalizes a slash-separated path in place: ensures a leading '/',
// collapses repeated '/', and resolves '.' / '..' segments within cap.
// cap is BOTH the work bound and the size of `path`: pass cap == sizeof(path).
// The function has no independent knowledge of path's real size, so a cap larger
// than the buffer would overflow on copy-back.
static inline void normalizePath(char *path, int cap)
{
   if (!path || cap <= 0) return;
   // cap < 2 leaves no room for the "/" + NUL the function always produces; the
   // empty-result fixup below would otherwise write path[0]='/' and path[1]=0,
   // a 1-byte overflow at cap==1. Terminate in place and return instead.
   if (cap < 2) { path[0] = 0; return; }

   // Stack-allocated temp buffer with reasonable limit to avoid VLA issues.
   char normalized[1024];
   if (cap > (int)sizeof(normalized)) cap = (int)sizeof(normalized);

   int normalizedLength = 0;
   int pathIndex = 0;
   int pathLength = getStrLen(path);

   if (path[0] != '/' && normalizedLength < cap - 1) normalized[normalizedLength++] = '/';

   while (pathIndex < pathLength) {
     while (pathIndex < pathLength && path[pathIndex] == '/') pathIndex++;
     if (pathIndex >= pathLength) break;

     int componentStart = pathIndex;
     while (pathIndex < pathLength && path[pathIndex] != '/') pathIndex++;
     int componentLength = pathIndex - componentStart;

     if (componentLength == 1 && path[componentStart] == '.') continue;

     if (componentLength == 2 && path[componentStart] == '.' && path[componentStart + 1] == '.') {
       if (normalizedLength > 1) {
         normalizedLength--;
         while (normalizedLength > 0 && normalized[normalizedLength - 1] != '/') normalizedLength--;
         if (normalizedLength > 1) normalizedLength--;
       }
       continue;
     }

     if ((normalizedLength == 0 || normalized[normalizedLength - 1] != '/') && normalizedLength < cap - 1)
       normalized[normalizedLength++] = '/';
     for (int index = 0; index < componentLength && normalizedLength < cap - 1; index++)
       normalized[normalizedLength++] = path[componentStart + index];
   }

   if (normalizedLength == 0) normalized[normalizedLength++] = '/';
   normalized[normalizedLength] = 0;
   memCopy(path, normalized, normalizedLength + 1);
}

// Case-insensitive string comparison.
static inline int strCmpICase(const char *a, const char *b)
{
   while (*a && *b) {
     char ca = *a, cb = *b;
     ca = toLowerChar(ca);
     cb = toLowerChar(cb);
     if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
     a++; b++;
   }
   return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// Returns 1 if s ends with suf (case-insensitive).
static inline int endsWithICase(const char *s, const char *suf)
{
   int ls = getStrLen(s), lsuf = getStrLen(suf);
   if (lsuf > ls) return 0;
   return strCmpICase(s + ls - lsuf, suf) == 0;
}

// Finds needle in hay. Returns offset or -1.
static inline int findBytes(const char *hay, int hayLength, const char *needle, int needleLength)
{
   if (needleLength == 0 || needleLength > hayLength) return -1;
   for (int i = 0; i <= hayLength - needleLength; i++) {
     int j = 0;
     while (j < needleLength && hay[i + j] == needle[j]) j++;
     if (j == needleLength) return i;
   }
   return -1;
}

// Appends src to dst at *off, respecting cap.
static inline void appendStr(char *dst, int cap, int *off, const char *src)
{
   int o = *off;
   for (int i = 0; src[i] && o < cap - 1; i++) dst[o++] = src[i];
   *off = o;
}

// Appends src to dst with XML entity escaping.
static inline void appendXmlEscaped(char *dst, int cap, int *off, const char *src)
{
   int o = *off;
   for (int i = 0; src[i] && o < cap - 8; i++) {
     unsigned char c = (unsigned char)src[i];
     const char *e = 0;
     if      (c == '&')  e = "&amp;";
     else if (c == '<')  e = "&lt;";
     else if (c == '>')  e = "&gt;";
     else if (c == '"') e = "&quot;";
     else if (c == 39)   e = "&apos;";
     if (e) while (*e && o < cap - 1) dst[o++] = *e++;
     else   dst[o++] = (char)c;
   }
   *off = o;
}

// Appends src percent-encoded (RFC 3986 unreserved set passes through).
static inline void appendUrlEnc(char *dst, int cap, int *off, const char *src)
{
   static const char hex[] = "0123456789ABCDEF";
   int o = *off;
   for (int i = 0; src[i] && o < cap - 4; i++) {
     unsigned char c = (unsigned char)src[i];
     int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
       (c >= '0' && c <= '9') ||
       c == '-' || c == '_' || c == '.' || c == '~';
     if (safe) {
       dst[o++] = (char)c;
     } else {
       dst[o++] = '%';
       dst[o++] = hex[c >> 4];
       dst[o++] = hex[c & 0x0F];
     }
   }
   *off = o;
}

// Returns 0..15 for a hex character, or -1 on invalid.
static inline int hexDigit(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

// Percent-decodes src into dst, NUL-terminating. Stops at space/'?'/'\r'/end.
// Inverse of appendUrlEnc. Returns chars written, or -1 on bad %XX.
static inline int urlDecode(const char *src, char *dst, int cap)
{
   int o = 0;
   while (*src && *src != ' ' && *src != '?' && *src != '\r' && o < cap - 1) {
     char c = *src++;
     if (c == '%') {
       // require both hex digits to be present before reading them: a trailing
       // bare '%' (or '%X') at the end of a non-NUL-terminated slice would
       // otherwise read one byte past the buffer. checking src[0] short-circuits
       // before hexDigit(src[1]) is evaluated.
       if (!src[0] || !src[1]) return -1;
       int hi = hexDigit(*src); if (hi < 0) return -1; src++;
       int lo = hexDigit(*src); if (lo < 0) return -1; src++;
       dst[o++] = (char)((hi << 4) | lo);
     } else if (c == '+') {
       dst[o++] = ' ';
     } else {
       dst[o++] = c;
     }
   }
   dst[o] = '\0';
   return o;
}

// Writes non-negative int as decimal. Returns chars written.
static inline int intToDec(int v, char *out)
{
   char tmp[12]; int t = 0;
   if (v == 0) tmp[t++] = '0';
   else while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
   int n = t;
   while (t--) *out++ = tmp[t];
   return n;
}

// Uppercases src into dst, NUL-terminated, bounded by cap. ASCII only.
static inline void toUpper(char *dst, int cap, const char *src)
{
   int i = 0;
   for (; i < cap - 1 && src[i]; i++) {
     dst[i] = toUpperChar(src[i]);
   }
   dst[i] = 0;
}

// NUL-terminated copy bounded by cap. Truncates rather than overruns.
static inline void strCopy(char *dst, int cap, const char *src)
{
   int i = 0;
   while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
   dst[i] = 0;
}

// Formats a unix timestamp as local time using the compact UI style "DD/MM/YY HH:MM".
// Returns 1 on success, 0 on conversion failure or insufficient output space.
static inline int formatDateTimeLocal(char *dst, int cap, uint64_t unixTime)
{
   if (!dst || cap < 15) {
     if (dst && cap > 0) dst[0] = 0;
     return 0;
   }

   CellRtcDateTime utc;
   CellRtcTick utcTick;
   CellRtcTick localTick;
   CellRtcDateTime local;
   if (cellRtcSetTime_t(&utc, unixTime) < 0 ||
      cellRtcGetTick(&utc, &utcTick) < 0 ||
      cellRtcConvertUtcToLocalTime(&utcTick, &localTick) < 0 ||
      cellRtcSetTick(&local, &localTick) < 0) {
       dst[0] = 0;
       return 0;
   }

   int p = 0;
   dst[p++] = '0' + (local.day / 10);
   dst[p++] = '0' + (local.day % 10);
   dst[p++] = '/';
   dst[p++] = '0' + (local.month / 10);
   dst[p++] = '0' + (local.month % 10);
   dst[p++] = '/';
   dst[p++] = '0' + ((local.year / 10) % 10);
   dst[p++] = '0' + (local.year % 10);
   dst[p++] = ' ';
   dst[p++] = '0' + (local.hour / 10);
   dst[p++] = '0' + (local.hour % 10);
   dst[p++] = ':';
   dst[p++] = '0' + (local.minute / 10);
   dst[p++] = '0' + (local.minute % 10);
   dst[p] = 0;
   return 1;
}

// Returns s, or "" if s is NULL. Useful for printf/setLabelText calls
// where a null pointer would otherwise crash or need a ternary at each call site.
static inline const char *strOrEmpty(const char *s) { return s ? s : ""; }

// Appends decimal uint64_t to buf at offset o; returns new offset.
static inline int appendUint64(char *buf, int cap, int o, uint64_t v)
{
   char num[24];
   int t = 0;
   if (v == 0) num[t++] = '0';
   else while (v) { num[t++] = (char)('0' + v % 10); v /= 10; }
   while (t > 0 && o < cap - 1) buf[o++] = num[--t];
   return o;
}

// Formats an IPv4 address (network byte order, as in sockaddr_in.s_addr) as
// dotted-decimal "a.b.c.d". NUL-terminates within cap; returns the string length.
static inline int formatIpv4(char *dst, int cap, uint32_t addr)
{
   if (!dst || cap < 16) { if (dst && cap > 0) dst[0] = 0; return 0; }
   int p = 0;
   for (int shift = 24; shift >= 0; shift -= 8) {
     p += intToDec((int)((addr >> shift) & 0xff), dst + p);
     if (shift) dst[p++] = '.';
   }
   dst[p] = 0;
   return p;
}

// Converts a UTF-8 string to UTF-16, emitting surrogate pairs for astral code
// points and skipping malformed bytes. Writes at most maxUnits code units plus a
// NUL terminator. The PS3 system APIs (e.g. cellOskDialog) speak UTF-16.
// `in` MUST be NUL-terminated: continuation-byte validation relies on the NUL to
// stop at a truncated trailing lead byte. A non-terminated slice would let a
// dangling multi-byte lead read one byte past the buffer -- wrap such input in a
// NUL-terminated copy first (urlDecode handles its own length-delimited input).
static inline void utf8ToUtf16(const char *in, uint16_t *out, int maxUnits)
{
   int n = 0;
   const unsigned char *s = (const unsigned char *)in;
   if (!s) { out[0] = 0; return; }

   while (*s && n < maxUnits) {
     uint32_t cp;
     if (*s < 0x80) {
       cp = *s++;
     } else if ((*s & 0xE0) == 0xC0) {
       cp = *s++ & 0x1F;
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
     } else if ((*s & 0xF0) == 0xE0) {
       cp = *s++ & 0x0F;
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
     } else if ((*s & 0xF8) == 0xF0) {
       cp = *s++ & 0x07;
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
       if ((*s & 0xC0) != 0x80) continue;  cp = (cp << 6) | (*s++ & 0x3F);
     } else {
       s++;  continue;
     }

     if (cp <= 0xFFFF) {
       out[n++] = (uint16_t)cp;
     } else if (n + 1 < maxUnits) {
       cp -= 0x10000;
       out[n++] = (uint16_t)(0xD800 | (cp >> 10));
       out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
     }
   }
   out[n] = 0;
}

// Converts a NUL-terminated UTF-16 string to UTF-8, decoding surrogate pairs.
// Writes a NUL-terminated result bounded by cap bytes. Inverse of utf8ToUtf16.
static inline void utf16ToUtf8(const uint16_t *in, char *out, int cap)
{
   int o = 0;
   if (!in) { out[0] = 0; return; }

   for (int i = 0; in[i] && o + 4 < cap; i++) {
     uint32_t cp = in[i];
     if (cp >= 0xD800 && cp <= 0xDBFF && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF)
       cp = 0x10000 + ((cp - 0xD800) << 10) + (in[++i] - 0xDC00);
     else if (cp >= 0xD800 && cp <= 0xDFFF)
       cp = 0xFFFD;   // unpaired surrogate: emit U+FFFD, not an ill-formed WTF-8 3-byte sequence

     if (cp < 0x80) {
       out[o++] = (char)cp;
     } else if (cp < 0x800) {
       out[o++] = (char)(0xC0 | (cp >> 6));
       out[o++] = (char)(0x80 | (cp & 0x3F));
     } else if (cp < 0x10000) {
       out[o++] = (char)(0xE0 | (cp >> 12));
       out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
       out[o++] = (char)(0x80 | (cp & 0x3F));
     } else {
       out[o++] = (char)(0xF0 | (cp >> 18));
       out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
       out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
       out[o++] = (char)(0x80 | (cp & 0x3F));
     }
   }
   out[o] = 0;
}
