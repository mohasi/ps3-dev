#pragma once

// minimal PARAM.SFO value reader. format: psdevwiki.com/ps3/PARAM.SFO — a little-endian
// header, an index table of 16-byte entries, a key-name table, then a value table. the
// 4-byte magic is "\0PSF". libc-free and allocation-free, so a vsh prx can use it.

#include <stdint.h>
#include "string-utilities.h"   // strEq

static inline uint32_t readSfoLE32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t readSfoLE16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// copy the string value of key into out (null-terminated, at most cap-1 chars). returns
// the length written, or -1 if the buffer isn't an SFO or the key is absent. every table
// offset is bounds-checked against len, so a malformed file can't read out of bounds.
static inline int getSfoValue(const uint8_t *sfo, uint64_t len, const char *key, char *out, int cap)
{
   if (cap < 1) return -1;
   out[0] = '\0';
   if (len < 0x14 || sfo[0] != 0x00 || sfo[1] != 0x50 || sfo[2] != 0x53 || sfo[3] != 0x46) return -1;   // "\0PSF"
   uint32_t keyTable  = readSfoLE32(sfo + 0x08);
   uint32_t dataTable = readSfoLE32(sfo + 0x0c);
   uint32_t entries   = readSfoLE32(sfo + 0x10);

   for (uint32_t i = 0; i < entries; i++) {
      uint64_t entry = 0x14 + (uint64_t)i * 0x10;
      if (entry + 0x10 > len) return -1;
      uint16_t keyOffset  = readSfoLE16(sfo + entry + 0x00);
      uint32_t dataLength = readSfoLE32(sfo + entry + 0x04);
      uint32_t dataOffset = readSfoLE32(sfo + entry + 0x0c);
      if ((uint64_t)keyTable + keyOffset >= len) return -1;
      if (!strEq((const char *)(sfo + keyTable + keyOffset), key)) continue;
      if ((uint64_t)dataTable + dataOffset + dataLength > len) return -1;

      const uint8_t *value = sfo + dataTable + dataOffset;
      int written = 0;
      while (written < (int)dataLength && written < cap - 1 && value[written]) { out[written] = (char)value[written]; written++; }
      out[written] = '\0';
      return written;
   }
   return -1;
}
