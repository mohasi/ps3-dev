#pragma once
//
// format.h - human-readable byte-size formatting. pure, libc-free, prx-safe;
// no filesystem dependency. split out of the old file.h (presentation, not I/O).
//
#include <stdint.h>
#include "string-utilities.h"   // appendUint64, getStrLen

// formats byte count as "1.23 MB" / "456 B" etc. buf must hold at least 24 bytes
// (a full TB value with fraction and unit fits well inside that).
static inline void formatSize(uint64_t bytes, char *buf)
{
   static const uint64_t thresh[] = { 1099511627776ULL, 1073741824ULL, 1048576ULL, 1024ULL };
   static const char *units[]     = { " TB",            " GB",         " MB",      " KB"  };
   int p = 0;
   for (int i = 0; i < 4; i++) {
      if (bytes >= thresh[i]) {
         // unsigned throughout: the whole part can exceed INT_MAX at TB scale,
         // so feed appendUint64 rather than casting uint64_t down to int.
         p = appendUint64(buf, 24, 0, bytes / thresh[i]);
         uint64_t frac = (bytes % thresh[i]) * 100 / thresh[i];
         buf[p++] = '.';
         buf[p++] = '0' + (frac / 10) % 10;
         buf[p++] = '0' + frac % 10;
         const char *u = units[i];
         while (*u) buf[p++] = *u++;
         buf[p] = '\0';
         return;
      }
   }
   p = appendUint64(buf, 24, 0, bytes);
   buf[p++] = ' '; buf[p++] = 'B'; buf[p] = '\0';
}

// like formatSize, but appends a trailing '+' when the byte count is only a
// lower bound (e.g. a folder walk that hit its time budget). buf must hold >= 24.
static inline void formatSizeApprox(uint64_t bytes, int approx, char *buf)
{
   formatSize(bytes, buf);
   if (!approx) return;
   int n = getStrLen(buf);
   buf[n]     = '+';
   buf[n + 1] = '\0';
}
