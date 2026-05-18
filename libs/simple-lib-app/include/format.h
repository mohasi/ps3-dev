#pragma once

// format - lightweight formatting and comparison helpers

// writes a non-negative int to buf, returns number of chars written
static inline int intToStr(int val, char *buf)
{
    if (val < 0) val = 0;
    int p = 0;
    char tmp[12];
    int t = 0;
    do { tmp[t++] = '0' + val % 10; val /= 10; } while (val > 0);
    while (t > 0) buf[p++] = tmp[--t];
    buf[p] = '\0';
    return p;
}

// case-insensitive string compare. returns <0, 0, or >0
static inline int compareStringNoCase(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
