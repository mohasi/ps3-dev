#include "rpk.h"

#include <string.h>
#include <stdlib.h>

#include "vfs.h"
#include "dbg.h"

// Little-endian byte assembly (file is LE; PPU is BE — never cast raw).
static uint32_t leU32(const unsigned char *b)
{
   return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t leU64(const unsigned char *b)
{
   uint64_t v = 0;
   for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
   return v;
}

static int readExact(VfsFile *f, void *buf, uint64_t want)
{
   int64_t got = readFs(f, buf, want);
   return (got == (int64_t)want) ? 0 : -1;
}

int openRpk(RpkFile *r, const char *path)
{
   memset(r, 0, sizeof(*r));

   if (openFs(path, VFS_O_RDONLY, &r->file) != 0) {
      logError("[rpk] cannot open %s\n", path);
      return -1;
   }

   unsigned char hdr[12];
   if (readExact(&r->file, hdr, 12) != 0) {
      logError("[rpk] short header\n");
      closeFs(&r->file);
      return -2;
   }
   if (memcmp(hdr, "RPK1", 4) != 0) {
      logError("[rpk] bad magic (not an .rpk)\n");
      closeFs(&r->file);
      return -3;
   }

   r->open = 1;
   r->version = leU32(hdr + 4);
   r->count = leU32(hdr + 8);
   logInfo("[rpk] opened: version=%u entries=%u\n", r->version, r->count);
   return 0;
}

void closeRpk(RpkFile *r)
{
   if (r->open) { closeFs(&r->file); r->open = 0; }
}

int readRpkInfo(RpkFile *r, char *manifestOut, int cap, long *gameRbcLen)
{
   if (!r->open) return -1;
   if (cap > 0) manifestOut[0] = '\0';
   if (gameRbcLen) *gameRbcLen = -1;

   uint64_t manOff = 0, manLen = 0;
   int haveMan = 0;

   // TOC starts right after the 12-byte header (file is positioned there).
   for (uint32_t i = 0; i < r->count; i++) {
      unsigned char lenB[4];
      if (readExact(&r->file, lenB, 4) != 0) { logError("[rpk] TOC nameLen @%u\n", i); return -2; }
      uint32_t nameLen = leU32(lenB);
      if (nameLen > 255) { logError("[rpk] absurd nameLen %u\n", nameLen); return -3; }

      char name[256];
      if (readExact(&r->file, name, nameLen) != 0) { logError("[rpk] TOC name @%u\n", i); return -4; }
      name[nameLen] = '\0';

      unsigned char ol[16];
      if (readExact(&r->file, ol, 16) != 0) { logError("[rpk] TOC off/len @%u\n", i); return -5; }
      uint64_t offset = leU64(ol);
      uint64_t length = leU64(ol + 8);

      if (strcmp(name, "manifest") == 0) { manOff = offset; manLen = length; haveMan = 1; }
      else if (strcmp(name, "game.rbc") == 0 && gameRbcLen) { *gameRbcLen = (long)length; }
   }

   if (!haveMan) { logWarn("[rpk] no manifest entry\n"); return 0; }

   if (seekFs(&r->file, (int64_t)manOff, VFS_SEEK_SET) < 0) {
      logError("[rpk] seek to manifest failed\n");
      return -6;
   }

   uint64_t toReadU = manLen;                                           // manLen is file-controlled (TOC)
   if (toReadU > (uint64_t)(cap - 1)) toReadU = (uint64_t)(cap - 1);     // clamp before the int cast (avoids negative toRead)
   int toRead = (int)toReadU;
   if (readExact(&r->file, manifestOut, (uint64_t)toRead) != 0) { logError("[rpk] manifest read\n"); return -7; }
   manifestOut[toRead] = '\0';

   logInfo("[rpk] manifest=%u bytes, game.rbc=%ld bytes\n", (uint32_t)manLen, gameRbcLen ? *gameRbcLen : -1);
   return 0;
}

int readRpkEntry(RpkFile *r, const char *name, unsigned char **outBuf, long *outLen)
{
   if (!r->open) return -1;
   *outBuf = NULL;
   *outLen = 0;

   if (seekFs(&r->file, 12, VFS_SEEK_SET) < 0) return -2;  // TOC starts after header

   uint64_t foundOff = 0, foundLen = 0;
   int found = 0;
   for (uint32_t i = 0; i < r->count; i++)
   {
      unsigned char lenB[4];
      if (readExact(&r->file, lenB, 4) != 0) return -3;
      uint32_t nameLen = leU32(lenB);
      if (nameLen > 255) return -4;

      char nm[256];
      if (readExact(&r->file, nm, nameLen) != 0) return -5;
      nm[nameLen] = '\0';

      unsigned char ol[16];
      if (readExact(&r->file, ol, 16) != 0) return -6;
      if (!found && strcmp(nm, name) == 0) { foundOff = leU64(ol); foundLen = leU64(ol + 8); found = 1; }
   }
   if (!found) { logError("[rpk] entry not found: %s\n", name); return -7; }

   if (seekFs(&r->file, (int64_t)foundOff, VFS_SEEK_SET) < 0) return -8;

   if (foundLen > 0xFFFFFFFFu) return -9;   // absurd length: reject before (size_t) truncation on the 32-bit target
   unsigned char *buf = (unsigned char *)malloc((size_t)foundLen);
   if (!buf) return -9;
   if (readExact(&r->file, buf, foundLen) != 0) { free(buf); return -10; }

   *outBuf = buf;
   *outLen = (long)foundLen;
   logInfo("[rpk] read entry %s: %ld bytes\n", name, (long)foundLen);
   return 0;
}

static int endsWithCI(const char *s, const char *suffix)
{
   int ls = (int)strlen(s), lf = (int)strlen(suffix);
   if (lf > ls) return 0;
   const char *a = s + (ls - lf);
   for (int i = 0; i < lf; i++)
   {
      char ca = a[i], cb = suffix[i];
      if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
      if (ca != cb) return 0;
   }
   return 1;
}

int readRpkEntrySuffix(RpkFile *r, const char *suffix, int index, char *outName, int outNameCap,
                       unsigned char **outBuf, long *outLen)
{
   if (!r->open) return -1;
   *outBuf = NULL; *outLen = 0;
   if (outNameCap > 0) outName[0] = '\0';

   if (seekFs(&r->file, 12, VFS_SEEK_SET) < 0) return -2;

   uint64_t foundOff = 0, foundLen = 0;
   char foundName[256];
   int found = 0, matchN = 0;
   for (uint32_t i = 0; i < r->count && !found; i++)
   {
      unsigned char lenB[4];
      if (readExact(&r->file, lenB, 4) != 0) return -3;
      uint32_t nameLen = leU32(lenB);
      if (nameLen > 255) return -4;

      char nm[256];
      if (readExact(&r->file, nm, nameLen) != 0) return -5;
      nm[nameLen] = '\0';

      unsigned char ol[16];
      if (readExact(&r->file, ol, 16) != 0) return -6;
      if (endsWithCI(nm, suffix))
      {
         if (matchN == index)
         {
            foundOff = leU64(ol); foundLen = leU64(ol + 8);
            strncpy(foundName, nm, sizeof foundName - 1); foundName[sizeof foundName - 1] = '\0';
            found = 1;
         }
         matchN++;
      }
   }
   if (!found) return -7;

   if (seekFs(&r->file, (int64_t)foundOff, VFS_SEEK_SET) < 0) return -8;
   if (foundLen > 0xFFFFFFFFu) return -9;   // absurd length: reject before (size_t) truncation on the 32-bit target
   unsigned char *buf = (unsigned char *)malloc((size_t)foundLen);
   if (!buf) return -9;
   if (readExact(&r->file, buf, foundLen) != 0) { free(buf); return -10; }

   *outBuf = buf;
   *outLen = (long)foundLen;
   if (outNameCap > 0) { strncpy(outName, foundName, outNameCap - 1); outName[outNameCap - 1] = '\0'; }
   return 0;
}
