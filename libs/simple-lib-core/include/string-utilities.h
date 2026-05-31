#pragma once

#include <stdint.h>

// utf-8 em dash (U+2014). used in ui as a "value not known yet" placeholder
// (e.g. folder size while the background sizer is still walking).
#define EM_DASH "\xe2\x80\x94"

// Returns length of a string.
static inline int strLen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

// Byte-wise copy. Use instead of libc memcpy in code that links against a
// stripped libc (e.g. vsh prx imports — memcpy is not exported there).
static inline void memCopy(void *dst, const void *src, int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

// Strict string equality. Same reason as memCopy: avoids libc strcmp.
static inline int strEq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

// Case-insensitive string comparison.
static inline int strCmpICase(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// Returns 1 if s ends with suf (case-insensitive).
static inline int endsWithICase(const char *s, const char *suf)
{
    int ls = strLen(s), lsuf = strLen(suf);
    if (lsuf > ls) return 0;
    return strCmpICase(s + ls - lsuf, suf) == 0;
}

// Finds needle in hay. Returns offset or -1.
static inline int findBytes(const char *hay, int hLen, const char *needle, int nLen)
{
    if (nLen == 0 || nLen > hLen) return -1;
    for (int i = 0; i <= hLen - nLen; i++) {
        int j = 0;
        while (j < nLen && hay[i + j] == needle[j]) j++;
        if (j == nLen) return i;
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
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
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
    while (t-- && o < cap) buf[o++] = num[t];
    return o;
}
