#pragma once

// homebrew pkg install / uninstall for the bridge.
//
// install:  receive .pkg over the socket, stage to /dev_hdd0/packages/,
//           extract into /dev_hdd0/game/<TITLE_ID>/. self-contained pkg
//           parser (no game_ext_plugin, no install dialog).
// uninstall: recursive rm of /dev_hdd0/game/<TITLE_ID>/.
//
// scope: **debug-format pkgs only** (pkg_rev_type == 0x00000001). that is
// what the public sdk `make_package_npdrm` produces, and what this bridge
// is for (debugging our own homebrew). retail-format (0x80000001) pkgs
// are rejected -- they require sony's internal finalizer, no one builds
// them, and a debugging tool would never see one.
//
// title-id validation is strict on purpose -- it is interpolated into a
// filesystem path. nine chars, [A-Z0-9_], matches the npdrm spec. anything
// else is rejected before touching the fs.

#include <stdint.h>
#include "vfs.h"
#include "fileio.h"
#include "sha1.h"
#include "dbg.h"
#include "string-utilities.h"

#define PKG_STAGE_DIR     "/dev_hdd0/packages"
#define PKG_GAME_DIR      "/dev_hdd0/game"
#define PKG_TITLE_LEN     9
#define PKG_MAGIC         0x7F504B47u
#define PKG_REV_DEBUG     0x00000001u   // (revision DEBUG << 16) | (type PS3)
#define PKG_NAME_MAX      512
#define PKG_BODY_CHUNK    4096
#define PKG_TABLE_MAX     0x4000   // == 512 entries; way more than any homebrew pkg ships.

// 0x80-byte big-endian header at file offset 0.
typedef struct {
   uint32_t magic;
   uint32_t pkgRevType;
   uint32_t pkgInfoOffset;
   uint32_t pkgInfoCount;
   uint32_t pkgInfoSize;
   uint32_t itemCount;
   uint64_t pkgSize;
   uint64_t dataOffset;
   uint64_t dataSize;
   char     contentId[48];
   uint8_t  qaDigest[16];   // seeds the debug keystream
} PkgHeader;

// 0x20-byte entry inside the encrypted data section. one per file/dir.
typedef struct {
   uint32_t nameOffset;   // relative to dataOffset
   uint32_t nameSize;
   uint64_t fileOffset;   // relative to dataOffset
   uint64_t fileSize;
   uint32_t type;         // low 8 bits: 3=regular, 4=dir, 1=npdrm, 9=sdat
   uint32_t pad;
} PkgFileEntry;

static inline uint32_t readBE32(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint64_t readBE64(const uint8_t *p)
{
   return ((uint64_t)readBE32(p) << 32) | (uint64_t)readBE32(p + 4);
}
static inline uint32_t readLE32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t readLE16(const uint8_t *p)
{
   return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void parsePkgHeader(const uint8_t *raw, PkgHeader *h)
{
   h->magic         = readBE32(raw + 0x00);
   h->pkgRevType    = readBE32(raw + 0x04);
   h->pkgInfoOffset = readBE32(raw + 0x08);
   h->pkgInfoCount  = readBE32(raw + 0x0c);
   h->pkgInfoSize   = readBE32(raw + 0x10);
   h->itemCount     = readBE32(raw + 0x14);
   h->pkgSize       = readBE64(raw + 0x18);
   h->dataOffset    = readBE64(raw + 0x20);
   h->dataSize      = readBE64(raw + 0x28);
   memCopy(h->contentId, raw + 0x30, 48);
   memCopy(h->qaDigest,  raw + 0x60, 16);
}

// parse one pkg file entry from a 32-byte raw record.
static void parsePkgEntry(const uint8_t *raw, PkgFileEntry *e)
{
   e->nameOffset = readBE32(raw + 0x00);
   e->nameSize   = readBE32(raw + 0x04);
   e->fileOffset = readBE64(raw + 0x08);
   e->fileSize   = readBE64(raw + 0x10);
   e->type       = readBE32(raw + 0x18);
   e->pad        = readBE32(raw + 0x1c);
}

static int isValidTitleId(const char *id)
{
   for (int i = 0; i < PKG_TITLE_LEN; i++) {
      char c = id[i];
      int ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      if (!ok) return 0;
   }
   return id[PKG_TITLE_LEN] == '\0';
}

// /dev_hdd0/game/<titleId> -> out (caller-owned, cap bytes).
static void buildInstallPath(char *out, int cap, const char *titleId)
{
   snprintf(out, cap, "%s/%s", PKG_GAME_DIR, titleId);
}

// /dev_hdd0/packages/<name>.pkg -> out (caller-owned, cap bytes).
static void buildStagePath(char *out, int cap, const char *name)
{
   snprintf(out, cap, "%s/%s.pkg", PKG_STAGE_DIR, name);
}

// recursive rm of /dev_hdd0/game/<titleId>/. fills *bytesFreed with the
// total size of removed regular files. returns 0 on success, -1 on failure,
// 1 if the title was not installed.
static int uninstallPkg(const char *titleId, uint64_t *bytesFreed)
{
   char path[FILE_PATH_MAX];
   buildInstallPath(path, sizeof path, titleId);
   if (!fileExists(path)) return 1;
   return deleteTree(path, bytesFreed);
}

// receive `size` bytes from `cli` into /dev_hdd0/packages/<name>.pkg.
// returns 0 on success, -1 on io failure.
static int stagePkgUpload(int cli, const char *name, uint32_t size)
{
   char path[FILE_PATH_MAX];
   buildStagePath(path, sizeof path, name);
   makeDir(PKG_STAGE_DIR);
   return recvFile(cli, path, size);
}

// debug-pkg ctr cipher. each 16-byte block of keystream is the first 16
// bytes of sha1(64-byte schedule), where bytes 0x00..0x37 are derived from
// qa_digest and bytes 0x38..0x3f are the big-endian block counter:
//   schedule[0x00..0x07] = qa_digest[0x00..0x07]
//   schedule[0x08..0x0f] = qa_digest[0x00..0x07]
//   schedule[0x10..0x17] = qa_digest[0x08..0x0f]
//   schedule[0x18..0x1f] = qa_digest[0x08..0x0f]
//   schedule[0x20..0x37] = 0
//   schedule[0x38..0x3f] = be64(counter)
// counter starts at 0 for the first encrypted block.
typedef struct {
   uint8_t  schedule[64];  // first 56 bytes are constant per pkg
   uint64_t counter;       // block index, encoded into schedule[0x38..]
   uint8_t  keystream[16];
   int      keystreamPos;
} PkgCipher;

static void initPkgCipher(PkgCipher *c, const PkgHeader *h)
{
   for (int i = 0; i < 64; i++) c->schedule[i] = 0;
   memCopy(c->schedule + 0x00, h->qaDigest + 0, 8);
   memCopy(c->schedule + 0x08, h->qaDigest + 0, 8);
   memCopy(c->schedule + 0x10, h->qaDigest + 8, 8);
   memCopy(c->schedule + 0x18, h->qaDigest + 8, 8);
   c->counter = 0;
   c->keystreamPos = 16;
}

// generate the next 16-byte keystream block from c->counter, advance
// c->counter, and reset c->keystreamPos to 0.
static void producePkgCipherBlock(PkgCipher *c)
{
   for (int i = 0; i < 8; i++)
      c->schedule[0x38 + i] = (uint8_t)(c->counter >> ((7 - i) * 8));
   uint8_t digest[20];
   hashSha1(c->schedule, 64, digest);
   for (int i = 0; i < 16; i++) c->keystream[i] = digest[i];
   c->counter++;
   c->keystreamPos = 0;
}

static void seekPkgCipher(PkgCipher *c, uint64_t byteOffset)
{
   c->counter = byteOffset / 16;
   int partial = (int)(byteOffset % 16);
   if (partial == 0) {
      c->keystreamPos = 16;  // let next xcrypt produce the block
   } else {
      producePkgCipherBlock(c);
      c->keystreamPos = partial;
   }
}

static void xcryptPkgCipher(PkgCipher *c, uint8_t *buf, int len)
{
   while (len > 0) {
      if (c->keystreamPos >= 16) producePkgCipherBlock(c);
      int take = 16 - c->keystreamPos;
      if (take > len) take = len;
      for (int i = 0; i < take; i++) buf[i] ^= c->keystream[c->keystreamPos + i];
      c->keystreamPos += take;
      buf += take;
      len -= take;
   }
}

// read `len` bytes from `f` at absolute `offset` into `buf`. returns 0 on
// success, -1 on short read or io failure.
static int readPkgAt(VfsFile *f, uint64_t offset, void *buf, uint64_t len)
{
   if (seekFs(f, (int64_t)offset, VFS_SEEK_SET) < 0) return -1;
   uint8_t *p = (uint8_t *)buf;
   while (len > 0) {
      int64_t got = readFs(f, p, len);
      if (got <= 0) return -1;
      p += got;
      len -= (uint64_t)got;
   }
   return 0;
}

// decrypt a range of the data section into `out`. dataRelOffset is
// measured from header->dataOffset (== 0 for the file table).
static int readPkgDecrypted(VfsFile *f, const PkgHeader *h, uint64_t dataRelOffset,
                            void *out, uint64_t len)
{
   if (readPkgAt(f, h->dataOffset + dataRelOffset, out, len) < 0) return -1;
   PkgCipher c;
   initPkgCipher(&c, h);
   seekPkgCipher(&c, dataRelOffset);
   uint8_t *p = (uint8_t *)out;
   while (len > 0) {
      int take = len > 0x10000 ? 0x10000 : (int)len;
      xcryptPkgCipher(&c, p, take);
      p += take;
      len -= take;
   }
   return 0;
}

// stream-decrypt [dataRelOffset, dataRelOffset+len) from the pkg into the
// open output `outFd`. used for file bodies -- avoids buffering whole files
// in ram.
static int writePkgDecryptedToFile(VfsFile *f, const PkgHeader *h, uint64_t dataRelOffset,
                                   VfsFile *outF, uint64_t len)
{
   if (seekFs(f, (int64_t)(h->dataOffset + dataRelOffset), VFS_SEEK_SET) < 0) return -1;

   PkgCipher c;
   initPkgCipher(&c, h);
   seekPkgCipher(&c, dataRelOffset);

   static uint8_t chunk[PKG_BODY_CHUNK];
   while (len > 0) {
      uint64_t want = len > sizeof chunk ? sizeof chunk : len;
      int64_t got = readFs(f, chunk, want);
      if (got <= 0) return -1;
      xcryptPkgCipher(&c, chunk, (int)got);
      if (writeFs(outF, chunk, (uint64_t)got) != got) return -1;
      len -= (uint64_t)got;
   }
   return 0;
}

// minimal PARAM.SFO TITLE_ID reader. format: psdevwiki.com/ps3/PARAM.SFO.
// header is little-endian after the 4-byte magic.
static int readSfoTitleId(const uint8_t *sfo, uint64_t len, char *outTitleId)
{
   if (len < 0x14) return -1;
   if (readBE32(sfo) != 0x00505346u) return -1;        // "\0PSF"
   uint32_t keyTable  = readLE32(sfo + 0x08);
   uint32_t dataTable = readLE32(sfo + 0x0c);
   uint32_t entries   = readLE32(sfo + 0x10);

   for (uint32_t i = 0; i < entries; i++) {
      uint64_t entOff = 0x14 + (uint64_t)i * 0x10;
      if (entOff + 0x10 > len) return -1;
      uint16_t keyOff  = readLE16(sfo + entOff + 0x00);
      uint32_t dataLen = readLE32(sfo + entOff + 0x04);
      uint32_t dataOff = readLE32(sfo + entOff + 0x0c);
      // 64-bit offset math so a near-4G table offset can't wrap the bound check
      // (harmless on the 32-bit target where the pointer wraps in step, but
      // correct on any target and clearer intent).
      if ((uint64_t)keyTable + keyOff >= len) return -1;
      const char *key = (const char *)(sfo + keyTable + keyOff);
      if (!strEq(key, "TITLE_ID")) continue;
      if ((uint64_t)dataTable + dataOff + dataLen > len) return -1;
      uint32_t n = dataLen < PKG_TITLE_LEN ? dataLen : PKG_TITLE_LEN;
      memCopy(outTitleId, sfo + dataTable + dataOff, n);
      outTitleId[PKG_TITLE_LEN] = '\0';
      return 0;
   }
   return -1;
}

// Reject an entry name that could escape destDir. Names legitimately contain '/'
// subdirectories (e.g. USRDIR/EBOOT.BIN), so '/' is allowed - but an absolute
// path or any ".." path component is not. The entry name is decrypted from the
// untrusted pkg, so this is the trust boundary for the extract path.
static int isSafePkgEntryName(const char *name)
{
   if (name[0] == '/' || name[0] == '\\') return 0;   // absolute
   const char *p = name;
   for (;;) {
      // p is at the start of a path component
      if (p[0] == '.' && p[1] == '.' &&
          (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) return 0;   // ".." component
      while (*p && *p != '/' && *p != '\\') p++;     // skip to component end
      if (!*p) return 1;
      while (*p == '/' || *p == '\\') p++;           // skip separators
   }
}

// extract one entry to <destDir>/<name>. type low byte: 4 = dir (mkdir
// recursively, no body), 1/2/3/9 = file (write bytes). intermediate dirs
// in `name` are auto-created.
static int extractPkgEntry(VfsFile *pkgF, const PkgHeader *h, const PkgFileEntry *e,
                           const char *destDir)
{
   if (e->nameSize == 0 || e->nameSize >= PKG_NAME_MAX) return -1;
   // bound the entry's name/data ranges to the decrypted data section so a malformed
   // table can't seek/read/extract outside it (uint64 math, no overflow).
   if (e->nameOffset > h->dataSize || e->nameSize > h->dataSize - e->nameOffset) return -1;
   if (e->fileOffset > h->dataSize || e->fileSize > h->dataSize - e->fileOffset) return -1;
   char name[PKG_NAME_MAX];
   if (readPkgDecrypted(pkgF, h, e->nameOffset, name, e->nameSize) < 0) return -1;
   name[e->nameSize] = '\0';
   if (!isSafePkgEntryName(name)) return -1;          // reject path traversal

   char full[FILE_PATH_MAX];
   if (snprintf(full, sizeof full, "%s/%s", destDir, name) >= (int)sizeof full) return -1;

   // mkdir -p on parent dirs (and on `full` itself when it's a directory entry).
   for (int i = 0; full[i]; i++) {
      if (full[i] == '/') {
         full[i] = '\0';
         if (i > 0) makeDir(full);
         full[i] = '/';
      }
   }

   uint32_t low = e->type & 0xff;
   if (low == 4) return makeDir(full);  // directory entry

   VfsFile outF;
   if (openFs(full, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &outF) != 0) return -1;
   int wr = writePkgDecryptedToFile(pkgF, h, e->fileOffset, &outF, e->fileSize);
   // fold the close: a deferred commit error means the extracted file isn't durable.
   if (closeFs(&outF) != 0) wr = -1;
   return wr;
}

// open `pkgPath`, validate the header, and decrypt the file table into
// `*tableOut`. on success the caller owns `*fdOut` and must close it.
// `*tableOut` is a static buffer inside this header (the bridge dispatcher
// is single-threaded so this is safe). returns 0 or -1 (logged).
static int openPkgAndReadTable(const char *pkgPath, VfsFile *fileOut, PkgHeader *h, uint8_t **tableOut)
{
   if (openFs(pkgPath, VFS_O_RDONLY, fileOut) != 0) {
      logError("[pkg] open failed: %s\n", pkgPath);
      return -1;
   }

   uint8_t raw[0x80];
   if (readPkgAt(fileOut, 0, raw, sizeof raw) < 0) {
      closeFs(fileOut); logError("[pkg] header read failed\n"); return -1;
   }
   parsePkgHeader(raw, h);

   if (h->magic != PKG_MAGIC) {
      closeFs(fileOut); logError("[pkg] bad magic: 0x%08x\n", (unsigned)h->magic); return -1;
   }
   if (h->pkgRevType != PKG_REV_DEBUG) {
      closeFs(fileOut);
      logError("[pkg] unsupported pkg type: 0x%08x (debug pkgs only)\n", (unsigned)h->pkgRevType);
      return -1;
   }

   uint64_t tableBytes = (uint64_t)h->itemCount * 0x20;
   if (tableBytes > PKG_TABLE_MAX) {
      closeFs(fileOut); logError("[pkg] item count too large: %u\n", (unsigned)h->itemCount); return -1;
   }
   static uint8_t table[PKG_TABLE_MAX];
   if (readPkgDecrypted(fileOut, h, 0, table, tableBytes) < 0) {
      closeFs(fileOut); logError("[pkg] file table read failed\n"); return -1;
   }

   *tableOut = table;
   return 0;
}

// extract the staged pkg at `pkgPath` into `destDir`. on success fills
// *filesOut and *bytesOut. returns 0, or -1 on any failure (logged).
static int extractPkg(const char *pkgPath, const char *destDir,
                      uint32_t *filesOut, uint64_t *bytesOut)
{
   VfsFile f;
   PkgHeader h;
   uint8_t *table;
   if (openPkgAndReadTable(pkgPath, &f, &h, &table) < 0) return -1;

   makeDir(destDir);

   uint32_t fileCount = 0;
   uint64_t byteCount = 0;
   for (uint32_t i = 0; i < h.itemCount; i++) {
      PkgFileEntry e;
      parsePkgEntry(table + i * 0x20, &e);
      if (extractPkgEntry(&f, &h, &e, destDir) < 0) {
         closeFs(&f);
         logError("[pkg] entry %u extract failed\n", (unsigned)i);
         return -1;
      }
      if ((e.type & 0xff) != 4) {
         fileCount++;
         byteCount += e.fileSize;
      }
   }
   closeFs(&f);

   if (filesOut) *filesOut = fileCount;
   if (bytesOut) *bytesOut = byteCount;
   return 0;
}

// read TITLE_ID from PARAM.SFO inside the staged pkg without extracting
// anything. used by installPkg to derive the install dir before touching
// /dev_hdd0/game/. linear scan of the file table for "PARAM.SFO".
static int readPkgTitleId(const char *pkgPath, char *outTitleId)
{
   VfsFile f;
   PkgHeader h;
   uint8_t *table;
   if (openPkgAndReadTable(pkgPath, &f, &h, &table) < 0) return -1;

   int rc = -1;
   for (uint32_t i = 0; i < h.itemCount; i++) {
      PkgFileEntry e;
      parsePkgEntry(table + i * 0x20, &e);
      if (e.nameSize == 0 || e.nameSize >= PKG_NAME_MAX) continue;
      char name[PKG_NAME_MAX];
      if (readPkgDecrypted(&f, &h, e.nameOffset, name, e.nameSize) < 0) break;
      name[e.nameSize] = '\0';
      if (!strEq(name, "PARAM.SFO")) continue;
      if (e.fileSize > 0x4000) break;
      static uint8_t sfoBuf[0x4000];
      if (readPkgDecrypted(&f, &h, e.fileOffset, sfoBuf, e.fileSize) == 0) {
         rc = readSfoTitleId(sfoBuf, e.fileSize, outTitleId);
      }
      break;
   }
   closeFs(&f);
   return rc;
}

// Tickle the file that explore_plugin writes after a manual cfw install
// (/dev_hdd0/tmp/explore/xil2/reg.xml). Hypothesis: the xmb watches this
// file's mtime and re-scans /dev_hdd0/game/ when it changes, so bumping
// its timestamp is enough to register a freshly-extracted title.
// Payload format mirrors the captured 81-byte file exactly.
static int pokeXmbRefresh(void)
{
   makeDir("/dev_hdd0/tmp");
   makeDir("/dev_hdd0/tmp/explore");
   makeDir("/dev_hdd0/tmp/explore/xil2");

   CellRtcDateTime d;
   if (cellRtcGetCurrentClockUtc(&d) != 0) {
      logError("[pkg] pokeXmbRefresh: rtc query failed\n");
      return -1;
   }

   // exactly 81 bytes (matches captured reg.xml byte-for-byte in shape):
   // <?xml version="1.0" encoding="utf-8"?>\n<reg updated="YYYY-MM-DDTHH:MM:SS.SSZ" />\n
   char xml[96];
   int n = snprintf(xml, sizeof xml,
       "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
       "<reg updated=\"%04d-%02d-%02dT%02d:%02d:%02d.%02uZ\" />\n",
       (int)d.year, (int)d.month, (int)d.day,
       (int)d.hour, (int)d.minute, (int)d.second,
       (unsigned)((d.microsecond / 10000u) % 100u));
   if (n <= 0) return -1;

   if (writeFile("/dev_hdd0/tmp/explore/xil2/reg.xml", xml, (uint64_t)n) < 0) {
      logError("[pkg] pokeXmbRefresh: write reg.xml failed\n");
      return -1;
   }
   logInfo("[pkg] pokeXmbRefresh: wrote reg.xml (%d bytes)\n", n);
   return 0;
}

// full install: read title-id from the staged pkg sfo, optionally
// uninstall the existing tree, extract into /dev_hdd0/game/<TITLE_ID>/.
// fills *outTitleId / counts on success. returns 0, or -1 (logged).
static int installPkg(const char *pkgPath, int clean, char *outTitleId,
                      uint32_t *filesOut, uint64_t *bytesOut)
{
   if (readPkgTitleId(pkgPath, outTitleId) < 0) {
      logError("[pkg] could not read TITLE_ID from %s\n", pkgPath);
      return -1;
   }
   if (!isValidTitleId(outTitleId)) {
      logError("[pkg] invalid TITLE_ID '%s'\n", outTitleId);
      return -1;
   }
   if (clean) {
      uint64_t freed = 0;
      if (uninstallPkg(outTitleId, &freed) < 0) {
         logError("[pkg] pre-install uninstall failed for %s\n", outTitleId);
         return -1;
      }
   }
   char destDir[FILE_PATH_MAX];
   buildInstallPath(destDir, sizeof destDir, outTitleId);
   int rc = extractPkg(pkgPath, destDir, filesOut, bytesOut);
   if (rc == 0) pokeXmbRefresh();
   return rc;
}
