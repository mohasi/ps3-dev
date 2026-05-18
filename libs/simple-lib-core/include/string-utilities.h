#pragma once

// Returns length of a string.
static inline int strLen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

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
