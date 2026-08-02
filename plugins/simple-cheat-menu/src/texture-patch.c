#include "texture-patch.h"
#include "game-mem.h"
#include "vfs.h"
#include "dbg.h"
#include "printf.h"

#define TAG "[tex] "
#define VIDEO_BASE  0xc0000000u   // ppu-visible base of local (video) memory
#define DUMP_ROOT     "/dev_hdd0/tmp/simple-cheat-menu/dumps"     // per-title dump goes in DUMP_ROOT/<titleId>
#define PATCHES_ROOT  "/dev_hdd0/tmp/simple-cheat-menu/patches"   // a patch lives in PATCHES_ROOT/<titleId>/<name>
#define APPLIED_ROOT  "/dev_hdd0/tmp/simple-cheat-menu/applied"   // per-title: original-texture snapshots for revert

// texture-struct scan: sweep the game's main memory for persistent 24-byte CellGcmTexture records. we
// do NOT hard-code per-game regions — a fixed set only matched one game (Tokyo Jungle) and reading the
// wrong addresses in another game wedged lv2. instead we probe the address space in 64KB blocks (lv2's
// minimum user-mapping unit), and finely scan only the blocks that are actually mapped. video memory
// (0xc0000000+) holds pixel data, not structs, so the sweep stops below it.
#define STRUCT_SCAN_START  0x00010000u   // low end of user memory
#define STRUCT_SCAN_END    0xc0000000u   // RSX video base: structs live below it, pixels above
#define PROBE_BLOCK        0x00010000u   // 64KB: one page probe tells whether the whole block is mapped

#define SCAN_CHUNK   (16 * 1024)   // bulk read size for a validated texture's pixels
#define SCAN_PAGE    4096          // scan/read granularity: one aligned page never straddles a mapping hole
#define TEX_STRUCT_SIZE   24       // sizeof(CellGcmTexture): the record length we scan for

// FIFO words are stored native big-endian on the ppu: CELL_GCM_ENDIAN_SWAP is a
// no-op unless CELL_GCM_LITTLE_ENDIAN (it isn't here), so read them big-endian.
static inline uint32_t gcmWord(const unsigned char *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static unsigned char scanBuffer[SCAN_CHUNK];   // file-static: ~16 KB, no runtime alloc

// a per-buffer matcher: scan [buffer, buffer+length), return hits found.
typedef int (*BufferMatcher)(const unsigned char *buffer, int length);

// scan [start,end) one page-aligned page at a time and run match on each. reading a single aligned page
// can never straddle a mapped->unmapped boundary, so no read can trigger the ps3mapi all-or-nothing copy
// across an unmapped page — the operation that hard-locked lv2 when the scan ranges didn't match the game.
// a 24-byte struct can still straddle a page edge, so the tail of each contiguous page is carried into the
// front of the next page's scan window (dropped whenever a page is unmapped, i.e. the run is broken).
static int scanRegion(uint32_t pid, uint32_t start, uint32_t end, BufferMatcher match)
{
   const int carrySize = TEX_STRUCT_SIZE - 4;   // almost a whole struct: enough to catch one crossing the edge
   int found = 0, carry = 0;
   for (uint32_t addr = start & ~(SCAN_PAGE - 1); addr < end; addr += SCAN_PAGE) {
      if (readProcMem(pid, addr, scanBuffer + carry, SCAN_PAGE) != 0) { carry = 0; continue; }   // unmapped page: skip, break the run
      int length = carry + SCAN_PAGE;
      found += match(scanBuffer, length);
      for (int i = 0; i < carrySize; i++) scanBuffer[i] = scanBuffer[length - carrySize + i];   // straddle prefix for the next page
      carry = carrySize;
   }
   return found;
}

// sweep the game's mapped main memory and run the matcher across it. probe one page per 64KB block: a
// failed read means the whole block is unmapped (lv2 maps user memory in >=64KB units), so skip it
// cheaply; a mapped block gets a page-safe fine scan. game-agnostic — no fixed addresses — and safe:
// every read is a single aligned page, so none can straddle into unmapped memory.
static int sweepMappedMemory(uint32_t pid, BufferMatcher match)
{
   int total = 0, mappedBlocks = 0;
   uint32_t lowMapped = 0, highMapped = 0;   // extent of the game's mapped main memory (diagnostic)
   for (uint32_t block = STRUCT_SCAN_START; block < STRUCT_SCAN_END; ) {
      if (readProcMem(pid, block, scanBuffer, SCAN_PAGE) != 0) { block += PROBE_BLOCK; continue; }   // unmapped: skip
      // coalesce consecutive mapped blocks into one run, then scan it in a single page-safe pass so a
      // struct straddling an internal 64KB boundary isn't missed (scanRegion's carry spans those edges).
      uint32_t runStart = block;
      do { block += PROBE_BLOCK; mappedBlocks++; }
      while (block < STRUCT_SCAN_END && readProcMem(pid, block, scanBuffer, SCAN_PAGE) == 0);
      if (!lowMapped) lowMapped = runStart;
      highMapped = block;
      total += scanRegion(pid, runStart, block, match);
   }
   logInfo(TAG "sweep: %d mapped 64KB blocks [%x..%x], %d struct hits\n", mappedBlocks, lowMapped, highMapped, total);
   return total;
}

// ---- find persistent CellGcmTexture structs in the game's heap ----
// the game keeps a 24-byte CellGcmTexture per loaded texture in its own data; those persist regardless
// of when we pause, so they include the object/character art we want to dump and replace.

#define VIDEO_SIZE        0x0f900000u   // 0xcf900000 - 0xc0000000: local (video) memory span

// is this the format byte of a real texture? base format (flags LN/UN masked off)
// must be a known value. 0x81..0x9f are the normal bases; 0xad/0xae are their own.
static int isTextureFormat(uint8_t format)
{
   uint8_t base = format & 0x9f;   // strip CELL_GCM_TEXTURE_LN (0x20) / _UN (0x40)
   if (base >= 0x81 && base <= 0x9f) return 1;
   if (format == 0xad || format == 0xae) return 1;
   return 0;
}

// does a 24-byte CellGcmTexture struct sit at p, with every field sane? this is the whole false-positive
// defense: every field must be valid at once. the strongest discriminator is power-of-two dimensions —
// real GPU art is always power-of-two, while heap noise that happens to pass the other checks isn't
// (Resistance produced 255x769 / 253x257 / 4x1 junk that all die here).
static int looksLikeTextureStruct(const unsigned char *p, uint32_t *offsetOut, uint8_t *fmtOut, uint32_t *widthOut, uint32_t *heightOut, uint8_t *mipsOut)
{
   uint8_t  format    = p[0];
   uint8_t  mipmap    = p[1];
   uint8_t  dimension = p[2];
   uint8_t  cubemap   = p[3];
   uint32_t width     = ((uint32_t)p[8]  << 8) | p[9];
   uint32_t height    = ((uint32_t)p[10] << 8) | p[11];
   uint32_t depth     = ((uint32_t)p[12] << 8) | p[13];
   uint8_t  location  = p[14];
   uint32_t offset    = gcmWord(p + 20);

   if (!isTextureFormat(format))            return 0;
   if (mipmap == 0 || mipmap > 13)          return 0;
   if (dimension != 2)                      return 0;   // 2D art only (drops 1D/3D noise; moddable textures are 2D)
   if (cubemap > 1)                         return 0;
   if (location > 1)                        return 0;   // 0 local, 1 main
   if (depth != 1)                          return 0;   // a 2D texture has depth 1
   if (width < 8 || width > 4096)           return 0;
   if (height < 8 || height > 4096)         return 0;
   if ((width & (width - 1)) || (height & (height - 1))) return 0;   // power-of-two only: the noise killer
   if (location == 0 && offset >= VIDEO_SIZE) return 0;  // a video texture must point inside video memory

   *offsetOut = offset; *fmtOut = format; *widthOut = width; *heightOut = height; *mipsOut = mipmap;
   return 1;
}

// ---- dump texture pixels from video memory to disk, plus a manifest ----

#define MAX_DUMP         256             // unique textures held per scan
#define MAX_TEX_BYTES    (4u * 1024 * 1024)   // skip anything bigger (giant render targets)

typedef struct { uint32_t offset, size; uint16_t width, height; uint8_t fmt, mips, location; } TexEntry;
static TexEntry collected[MAX_DUMP];
static int collectedCount;

// bytes for one mip level of a texture (block-compressed formats round up to 4x4 blocks).
static uint32_t levelSize(uint8_t fmt, uint32_t width, uint32_t height)
{
   uint8_t base = fmt & 0x9f;
   uint32_t blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
   switch (base) {
      case 0x86: return blocksWide * blocksHigh * 8;    // DXT1
      case 0x87: case 0x88: return blocksWide * blocksHigh * 16;   // DXT23 / DXT45
      case 0x81: return width * height;                 // B8
      case 0x85: case 0x9e: return width * height * 4;  // A8R8G8B8 / D8R8G8B8
      case 0x82: case 0x83: case 0x84: case 0x8b: case 0x8f: case 0x95: case 0x97: case 0x9d:
         return width * height * 2;                      // 16-bit formats
      default: return width * height * 4;                // unknown: overestimate so we capture it all
   }
}

// full byte size across the mip chain.
static uint32_t textureByteSize(uint8_t fmt, uint32_t width, uint32_t height, uint8_t mips)
{
   uint32_t total = 0;
   for (uint8_t level = 0; level < mips; level++) {
      uint32_t w = width >> level, h = height >> level;
      total += levelSize(fmt, w ? w : 1, h ? h : 1);
   }
   return total;
}

// add a unique-by-offset texture to collected[] (shared by the struct and fifo collectors).
static void addCollectedTexture(uint32_t offset, uint8_t fmt, uint32_t width, uint32_t height, uint8_t mips, uint8_t location)
{
   if (collectedCount >= MAX_DUMP) return;
   for (int e = 0; e < collectedCount; e++) if (collected[e].offset == offset) return;
   TexEntry *entry = &collected[collectedCount++];
   entry->offset = offset; entry->fmt = fmt; entry->width = (uint16_t)width; entry->height = (uint16_t)height;
   entry->mips = mips; entry->location = location;
   entry->size = textureByteSize(fmt, width, height, mips);
}

// collector matcher: find persistent CellGcmTexture structs and add each unique one.
static int scanBufferCollect(const unsigned char *buffer, int length)
{
   int found = 0;
   for (int i = 0; i + TEX_STRUCT_SIZE <= length; i += 4) {
      uint32_t offset, width, height; uint8_t format, mips;
      if (!looksLikeTextureStruct(buffer + i, &offset, &format, &width, &height, &mips)) continue;
      found++;
      addCollectedTexture(offset, format, width, height, mips, buffer[i + 14]);   // loc: 0 video, 1 main
   }
   return found;
}

// ---- command-stream (FIFO) texture collector: for engines that don't keep persistent CellGcmTexture
// structs (e.g. Resistance), the textures they bind each frame are in the readable command stream. we scan
// for a SET_TEXTURE_OFFSET(_FORMAT) command (offset + format word) paired with the unit's IMAGE_RECT
// (width/height) — either the 7th register of a batched setup block, or a separate command a few words on
// (the standard cellGcmSetTexture path). the format+dims cross-check throws out data that merely looks like
// a command header. FIFO word format: header = (count<<18) | method; fields are big-endian on the ppu.

#define TEX_OFFSET_METHOD  0x1a00u   // CELL_GCM_NV4097_SET_TEXTURE_OFFSET; texture unit n = + n*0x20
#define TEX_RECT_METHOD    0x1a18u   // CELL_GCM_NV4097_SET_TEXTURE_IMAGE_RECT; unit n = + n*0x20
#define TEX_METHOD_STRIDE  0x20u
#define TEX_UNITS          16
#define RECT_LOOKAHEAD     10        // words after OFFSET_FORMAT to find a separate IMAGE_RECT command

static int scanBufferForFifoTextures(const unsigned char *buffer, int length)
{
   int words = length / 4;
   int found = 0;
   for (int w = 0; w + 2 < words; w++) {
      uint32_t header = gcmWord(buffer + w * 4);
      uint32_t count  = (header >> 18) & 0x7FF;
      uint32_t method = header & 0x1FFFC;
      if (count < 2 || count > 64) continue;
      if (method < TEX_OFFSET_METHOD || method >= TEX_OFFSET_METHOD + TEX_UNITS * TEX_METHOD_STRIDE) continue;
      if ((method - TEX_OFFSET_METHOD) % TEX_METHOD_STRIDE != 0) continue;   // exactly a unit's OFFSET register
      int unit = (int)((method - TEX_OFFSET_METHOD) / TEX_METHOD_STRIDE);

      uint32_t offset  = gcmWord(buffer + (w + 1) * 4);   // arg0 = texture offset
      uint32_t fmtWord = gcmWord(buffer + (w + 2) * 4);   // arg1 = format word

      // decode + validate the format word: (location+1) | cubemap<<2 | border<<3 | dimension<<4 | format<<8 | mipmap<<16
      int dmaContext = fmtWord & 3;                        // 1 = local (video), 2 = main
      if (dmaContext != 1 && dmaContext != 2) continue;
      uint8_t location  = (uint8_t)(dmaContext - 1);
      uint8_t dimension = (uint8_t)((fmtWord >> 4) & 0xf);
      uint8_t format    = (uint8_t)((fmtWord >> 8) & 0xff);
      uint8_t mipmap    = (uint8_t)((fmtWord >> 16) & 0xff);
      if (dimension != 2 || !isTextureFormat(format) || mipmap == 0 || mipmap > 13) continue;
      if (location == 0 && offset >= VIDEO_SIZE) continue;

      // image rect (width/height): the 7th register of a batched block, or a nearby separate command
      uint32_t rect = 0;
      uint32_t rectMethod = TEX_RECT_METHOD + (uint32_t)unit * TEX_METHOD_STRIDE;
      if (count >= 7 && w + 7 < words) {
         rect = gcmWord(buffer + (w + 7) * 4);
      } else {
         for (int k = w + 3; k <= w + RECT_LOOKAHEAD && k + 1 < words; k++) {
            uint32_t h = gcmWord(buffer + k * 4);
            if ((h & 0x1FFFC) == rectMethod && ((h >> 18) & 0x7FF) >= 1) { rect = gcmWord(buffer + (k + 1) * 4); break; }
         }
      }
      uint32_t width  = (rect >> 16) & 0xffff;
      uint32_t height = rect & 0xffff;
      if (width < 8 || width > 4096 || height < 8 || height > 4096) continue;
      if ((width & (width - 1)) || (height & (height - 1))) continue;   // power-of-two: the noise killer

      addCollectedTexture(offset, format, width, height, mipmap, location);
      found++;
   }
   return found;
}

// read one texture's bytes from video memory and write them to path. returns 0 on full success.
static int dumpOneTexture(uint32_t pid, const TexEntry *entry, const char *path)
{
   VfsFile file;
   if (openFs(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file) != 0) return -1;

   int rc = 0;
   for (uint32_t done = 0; done < entry->size; ) {
      uint32_t want = entry->size - done < SCAN_CHUNK ? entry->size - done : SCAN_CHUNK;
      if (readProcMem(pid, VIDEO_BASE + entry->offset + done, scanBuffer, want) != 0) { rc = -2; break; }
      if (writeFs(&file, scanBuffer, want) != (int64_t)want) { rc = -3; break; }
      done += want;
   }
   closeFs(&file);
   return rc;
}

// fill `collected` with the game's unique textures (shared by dump and overwrite). two sources, deduped by
// offset: persistent CellGcmTexture structs in the heap (engines that keep them), then the command-stream
// binds (engines that don't). a game that has structs gets them cheaply; one that doesn't still gets its
// textures from the FIFO.
static void collectTextures(uint32_t pid)
{
   collectedCount = 0;
   sweepMappedMemory(pid, scanBufferCollect);
   int fromStructs = collectedCount;
   sweepMappedMemory(pid, scanBufferForFifoTextures);
   logInfo(TAG "collect: %d struct + %d fifo = %d textures\n", fromStructs, collectedCount - fromStructs, collectedCount);
}

static uint32_t hashTextureBytes(uint32_t pid, uint32_t offset, uint32_t size);   // defined with the apply path

// write the already-collected textures to the title's dump folder: each video texture's pixels to
// DUMP_ROOT/<titleId>/<contentHash>.bin (named by content, so re-writing the same one is a no-op) and a
// line to that title's manifest. main-memory (location 1) textures are skipped — only video is read here.
// returns how many NEW files were written, or -1 if the manifest couldn't be opened.
static int writeCollectedTextures(uint32_t pid, const char *titleId)
{
   char dir[128];
   snprintf(dir, sizeof dir, "%s/%s", DUMP_ROOT, titleId);
   makeDirPath(DUMP_ROOT);   // makeDirPath is a single mkdir (not recursive), so make the root before the per-title subdir
   makeDirPath(dir);

   char manifestPath[160];
   snprintf(manifestPath, sizeof manifestPath, "%s/manifest.txt", dir);
   int freshManifest = !fileExists(manifestPath);
   VfsFile manifest;
   if (openFs(manifestPath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &manifest) != 0) {
      logError(TAG "dump: cannot open manifest\n"); return -1;
   }
   char line[96];
   if (freshManifest) { int n = snprintf(line, sizeof line, "# hash fmt width height mips size\n"); writeFs(&manifest, line, n); }

   int added = 0, skipLoc = 0, skipSize = 0, dup = 0, readFail = 0;
   for (int i = 0; i < collectedCount; i++) {
      TexEntry *entry = &collected[i];
      if (entry->location != 0) { skipLoc++; continue; }         // offset is into main memory, not video
      if (entry->size == 0 || entry->size > MAX_TEX_BYTES) { skipSize++; continue; }

      uint32_t hash = hashTextureBytes(pid, entry->offset, entry->size);
      char path[160];
      snprintf(path, sizeof path, "%s/%08x.bin", dir, hash);
      if (fileExists(path)) { dup++; continue; }                 // this content already dumped
      if (dumpOneTexture(pid, entry, path) != 0) { readFail++; continue; }   // read/write failed: skip

      int n = snprintf(line, sizeof line, "%08x %02x %u %u %u %u\n", hash, entry->fmt, entry->width, entry->height, entry->mips, entry->size);
      writeFs(&manifest, line, n);
      added++;
   }

   closeFs(&manifest);
   logInfo(TAG "dump: +%d new (%d collected) loc!=0:%d size:%d dup:%d readfail:%d -> %s\n",
           added, collectedCount, skipLoc, skipSize, dup, readFail, dir);
   return added;
}

// dump accumulates: the folder builds up the game's art as you scan it. one-shot: collect the on-screen
// textures now (heap structs + command-stream binds) and write them.
int dumpTextures(uint32_t pid, const char *titleId)
{
   if (!pid) { logError(TAG "no game running\n"); return 0; }
   if (!titleId || !titleId[0]) { logError(TAG "dump: no title id\n"); return 0; }

   collectTextures(pid);
   if (collectedCount == 0) { logInfo(TAG "dump: nothing collected\n"); return 0; }
   return writeCollectedTextures(pid, titleId);
}

// ---- apply a texture pack (replace matched textures by content fingerprint) ----
// each pack entry carries the ORIGINAL hash and the REPLACEMENT hash. at apply time we
// fingerprint the live texture: == original -> write the replacement; == replacement ->
// already applied, skip. content-keyed, so it survives reboots and texture reloads (M6).

#define FNV_BASIS   2166136261u   // matches the cheat-hash convention (overlay.cpp)
#define FNV_PRIME   16777619u
#define MAX_PACK    64
#define MAX_PARTS   32
#define PART_NAME   40
#define PACK_LINE   4096

typedef struct { uint32_t origHash, replHash; uint16_t width, height; uint8_t fmt; char file[48]; int part; } PackEntry;
static PackEntry pack[MAX_PACK];
static int packCount;

// optional parts: a patch may split its entries into named parts the menu turns on and off. a part in a
// pick-one group is mutually exclusive with the others in that group (variants); a pick-any part toggles
// freely. a manifest with no part lines leaves every entry at part == -1 and applies as one whole patch.
typedef struct { char name[PART_NAME]; int group; } Part;    // group: -1 standalone, else index into groups[]
typedef struct { char name[PART_NAME]; int pickOne; } Group; // pickOne: 1 = radio (variants), 0 = free toggles
static Part parts[MAX_PARTS];
static int partCount;
static Group groups[MAX_PARTS];
static int groupCount;

// fingerprint a texture's live bytes (FNV-1a over the whole blob). 0 = unreadable.
static uint32_t hashTextureBytes(uint32_t pid, uint32_t offset, uint32_t size)
{
   uint32_t hash = FNV_BASIS;
   for (uint32_t off = 0; off < size; ) {
      uint32_t want = size - off < SCAN_CHUNK ? size - off : SCAN_CHUNK;
      if (readProcMem(pid, VIDEO_BASE + offset + off, scanBuffer, want) != 0) return 0;
      for (uint32_t k = 0; k < want; k++) { hash ^= scanBuffer[k]; hash *= FNV_PRIME; }
      off += want;
   }
   return hash;
}

// stream a replacement .bin from the pack folder over a texture in video memory. 0 on success.
static int writeReplacement(uint32_t pid, uint32_t offset, uint32_t size, const char *packDir, const char *file)
{
   char path[160];
   snprintf(path, sizeof path, "%s/%s", packDir, file);
   VfsFile source;
   if (openFs(path, VFS_O_RDONLY, &source) != 0) return -1;

   int rc = 0;
   for (uint32_t off = 0; off < size; ) {
      uint32_t want = size - off < SCAN_CHUNK ? size - off : SCAN_CHUNK;
      int64_t got = readFs(&source, scanBuffer, want);
      if (got <= 0) break;   // file shorter than the texture: stop at what we have
      if (writeProcMem(pid, VIDEO_BASE + offset + off, scanBuffer, (uint32_t)got) != 0) { rc = -2; break; }
      off += (uint32_t)got;
   }
   closeFs(&source);
   return rc;
}

// tiny manifest-token parsers: advance `pos` past spaces, then read one field.
static void skipSpaces(const char *text, int *pos, int end) { while (*pos < end && (text[*pos] == ' ' || text[*pos] == '\t')) (*pos)++; }
static uint32_t parseHex(const char *text, int *pos, int end)
{
   skipSpaces(text, pos, end);
   uint32_t value = 0;
   while (*pos < end) {
      char c = text[*pos]; int digit;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      else break;
      value = (value << 4) | (uint32_t)digit; (*pos)++;
   }
   return value;
}
static uint32_t parseDec(const char *text, int *pos, int end)
{
   skipSpaces(text, pos, end);
   uint32_t value = 0;
   while (*pos < end && text[*pos] >= '0' && text[*pos] <= '9') { value = value * 10 + (uint32_t)(text[*pos] - '0'); (*pos)++; }
   return value;
}
static void parseToken(const char *text, int *pos, int end, char *out, int cap)
{
   skipSpaces(text, pos, end);
   int k = 0;
   while (*pos < end && text[*pos] > ' ' && k < cap - 1) out[k++] = text[(*pos)++];
   out[k] = 0;
}
// a part/group name runs to end-of-line so it can contain spaces ("Blue Dog"). trims trailing blanks.
static void parseLineRest(const char *text, int *pos, int end, char *out, int cap)
{
   skipSpaces(text, pos, end);
   int k = 0;
   while (*pos < end && text[*pos] != '\n' && text[*pos] != '\r' && k < cap - 1) out[k++] = text[(*pos)++];
   while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\t')) k--;
   out[k] = 0;
}
static int wordIs(const char *a, const char *b) { int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == b[i]; }

// read the pack manifest. texture lines are "origHash replHash width height fmt file". optional v2
// directive lines group those into parts: "group one|any <name>" opens a group, "part <name>" opens a
// part under the current group (or standalone), and each texture line joins the most recent part.
static void loadPack(const char *packDir)
{
   packCount = 0; partCount = 0; groupCount = 0;
   char manifestPath[160];
   snprintf(manifestPath, sizeof manifestPath, "%s/manifest.txt", packDir);
   char buffer[PACK_LINE];
   int bytes = readFile(manifestPath, buffer, sizeof buffer - 1);
   if (bytes <= 0) return;
   buffer[bytes] = 0;

   int currentGroup = -1, currentPart = -1;
   int pos = 0;
   while (pos < bytes && packCount < MAX_PACK) {
      while (pos < bytes && (buffer[pos] == '\n' || buffer[pos] == '\r' || buffer[pos] == ' ' || buffer[pos] == '\t')) pos++;
      if (pos >= bytes) break;
      if (buffer[pos] == '#') { while (pos < bytes && buffer[pos] != '\n') pos++; continue; }   // comment line

      // directive or texture line? the first token tells us (hashes are hex, never "group"/"part").
      int lineStart = pos;
      char keyword[8];
      parseToken(buffer, &pos, bytes, keyword, sizeof keyword);
      if (wordIs(keyword, "group") && groupCount < MAX_PARTS) {
         char kind[8];
         parseToken(buffer, &pos, bytes, kind, sizeof kind);
         groups[groupCount].pickOne = wordIs(kind, "one");
         parseLineRest(buffer, &pos, bytes, groups[groupCount].name, sizeof groups[0].name);
         currentGroup = groupCount++;
         currentPart = -1;   // entries wait for the group's first 'part'
         continue;
      }
      if (wordIs(keyword, "part") && partCount < MAX_PARTS) {
         parts[partCount].group = currentGroup;
         parseLineRest(buffer, &pos, bytes, parts[partCount].name, sizeof parts[0].name);
         currentPart = partCount++;
         continue;
      }

      pos = lineStart;   // a texture line: parse it positionally from the start
      PackEntry *entry = &pack[packCount];
      entry->origHash = parseHex(buffer, &pos, bytes);
      entry->replHash = parseHex(buffer, &pos, bytes);
      entry->width    = (uint16_t)parseDec(buffer, &pos, bytes);
      entry->height   = (uint16_t)parseDec(buffer, &pos, bytes);
      entry->fmt      = (uint8_t)parseHex(buffer, &pos, bytes);
      parseToken(buffer, &pos, bytes, entry->file, sizeof entry->file);
      entry->part = currentPart;
      if (entry->file[0]) packCount++;
      while (pos < bytes && buffer[pos] != '\n') pos++;   // to end of line
   }
}

// TEX_ORIGINAL = this live texture still holds the pack's original (an apply would replace it);
// TEX_REPLACED = it already holds the replacement (a revert would restore it). matchLiveTexture returns
// the pack entry index and which of these, or -1 if no entry matches this texture.
enum { TEX_ORIGINAL = 1, TEX_REPLACED = 2 };
static int matchLiveTexture(uint32_t pid, const TexEntry *tex, int *state)
{
   int dimsMatch = 0;   // fingerprint only if some pack entry shares this texture's dimensions/format
   for (int p = 0; p < packCount; p++)
      if (pack[p].width == tex->width && pack[p].height == tex->height && pack[p].fmt == tex->fmt) { dimsMatch = 1; break; }
   if (!dimsMatch) return -1;

   uint32_t live = hashTextureBytes(pid, tex->offset, tex->size);
   for (int p = 0; p < packCount; p++) {
      if (pack[p].width != tex->width || pack[p].height != tex->height || pack[p].fmt != tex->fmt) continue;
      if (live == pack[p].origHash) { *state = TEX_ORIGINAL; return p; }
      if (live == pack[p].replHash) { *state = TEX_REPLACED; return p; }
   }
   return -1;
}

// ---- snapshots: APPLIED_ROOT/<titleId> holds the original-texture bytes taken at apply time (named by
// origHash), so a patch can be turned off in-session by writing them back. which patches are on is kept
// in memory by the menu (like cheats), not on disk.

static void appliedDirOf(char *out, int cap, const char *titleId) { snprintf(out, cap, "%s/%s", APPLIED_ROOT, titleId); }

// game exited: its snapshots are stale (the process died and reloaded originals), so wipe them.
void clearAppliedState(const char *titleId)
{
   if (!titleId || !titleId[0]) return;
   char dir[160];
   appliedDirOf(dir, sizeof dir, titleId);
   uint64_t freed = 0;
   deleteTree(dir, &freed);
}

// ---- patches menu tab: list a title's patch folders, and apply one by name ----

int listPatchNames(const char *titleId, char *names, int nameCap, int maxNames)
{
   if (!titleId || !titleId[0]) return 0;

   char dir[128];
   snprintf(dir, sizeof dir, "%s/%s", PATCHES_ROOT, titleId);
   VfsDir handle;
   if (openDir(dir, &handle) != 0) return 0;   // no patches folder for this title yet

   int count = 0;
   char name[128];
   VfsEntryType type;
   while (count < maxNames && readDir(&handle, name, sizeof name, &type) == 1) {
      if (type != VFS_ENTRY_DIR || name[0] == '.') continue;   // each patch is a subfolder; skip . / ..
      char *slot = names + count * nameCap;
      int k = 0;
      while (name[k] && k < nameCap - 1) { slot[k] = name[k]; k++; }
      slot[k] = 0;
      count++;
   }
   closeDir(&handle);
   logInfo(TAG "patches: %d for %s\n", count, titleId);
   return count;
}

int applyPatch(uint32_t pid, const char *titleId, const char *patchName)
{
   if (!pid || !titleId || !titleId[0] || !patchName || !patchName[0]) { logError(TAG "apply: bad patch id\n"); return 0; }

   char packDir[192], snapDir[160];
   snprintf(packDir, sizeof packDir, "%s/%s/%s", PATCHES_ROOT, titleId, patchName);
   appliedDirOf(snapDir, sizeof snapDir, titleId);
   loadPack(packDir);
   if (packCount == 0) { logInfo(TAG "apply: no entries in %s\n", packDir); return 0; }

   makeDirPath(APPLIED_ROOT);
   makeDirPath(snapDir);
   collectTextures(pid);

   int applied = 0;
   for (int i = 0; i < collectedCount; i++) {
      TexEntry *tex = &collected[i];
      if (tex->location != 0) continue;

      int state;
      int p = matchLiveTexture(pid, tex, &state);
      if (p < 0 || state != TEX_ORIGINAL) continue;   // only overwrite a texture still holding the original

      // snapshot the original bytes (once) so the patch can be turned off later, then write the replacement
      char snapPath[224];
      snprintf(snapPath, sizeof snapPath, "%s/%08x.bin", snapDir, pack[p].origHash);
      if (!fileExists(snapPath)) dumpOneTexture(pid, tex, snapPath);
      if (writeReplacement(pid, tex->offset, tex->size, packDir, pack[p].file) == 0) {
         applied++;
         logInfo(TAG "apply: %ux%u -> %s\n", tex->width, tex->height, pack[p].file);
      }
   }

   logInfo(TAG "apply done: %s +%d\n", patchName, applied);
   return applied;
}

int revertPatch(uint32_t pid, const char *titleId, const char *patchName)
{
   if (!pid || !titleId || !titleId[0] || !patchName || !patchName[0]) { logError(TAG "revert: bad patch id\n"); return 0; }

   char packDir[192], snapDir[160];
   snprintf(packDir, sizeof packDir, "%s/%s/%s", PATCHES_ROOT, titleId, patchName);
   appliedDirOf(snapDir, sizeof snapDir, titleId);
   loadPack(packDir);

   collectTextures(pid);

   int reverted = 0;
   for (int i = 0; i < collectedCount; i++) {
      TexEntry *tex = &collected[i];
      if (tex->location != 0) continue;

      int state;
      int p = matchLiveTexture(pid, tex, &state);
      if (p < 0 || state != TEX_REPLACED) continue;   // only restore a texture that still holds our replacement

      char snapFile[24];
      snprintf(snapFile, sizeof snapFile, "%08x.bin", pack[p].origHash);
      if (writeReplacement(pid, tex->offset, tex->size, snapDir, snapFile) == 0) {
         reverted++;
         char snapPath[224];
         snprintf(snapPath, sizeof snapPath, "%s/%s", snapDir, snapFile);
         removeFilePath(snapPath);   // the original is back in the game; the snapshot is spent
      }
   }

   logInfo(TAG "revert done: %s -%d\n", patchName, reverted);
   return reverted;
}

// ---- parts: turn named pieces of one patch on and off with last-wins layering ----

// which replacement should show for a texture (identified by its original hash) under the current
// selection: the highest-index on entry sharing that hash (later manifest line wins), or -1 if none is
// on and the original should show. an entry with part == -1 (a whole patch) is always on.
static int getWinningEntry(uint32_t origHash, const unsigned char *partOn)
{
   int winner = -1;
   for (int e = 0; e < packCount; e++) {
      if (pack[e].origHash != origHash) continue;
      int on = pack[e].part < 0 ? 1 : (partOn && partOn[pack[e].part]);
      if (on) winner = e;
   }
   return winner;
}

// bring the game to the exact texture state implied by partOn (one flag per part). for every texture
// this patch touches, make the live bytes match the winning part's replacement, or the snapshotted
// original if no part wins. keyed on original content hash and recomputed from live state, so
// overlapping parts resolve last-wins and turning a part off reveals whatever should show underneath.
// returns how many textures now hold a replacement.
int rebuildPatch(uint32_t pid, const char *titleId, const char *patchName, const unsigned char *partOn)
{
   if (!pid || !titleId || !titleId[0] || !patchName || !patchName[0]) { logError(TAG "rebuild: bad patch id\n"); return 0; }

   char packDir[192], snapDir[160];
   snprintf(packDir, sizeof packDir, "%s/%s/%s", PATCHES_ROOT, titleId, patchName);
   appliedDirOf(snapDir, sizeof snapDir, titleId);
   loadPack(packDir);
   if (packCount == 0) { logInfo(TAG "rebuild: no entries in %s\n", packDir); return 0; }

   makeDirPath(APPLIED_ROOT);
   makeDirPath(snapDir);
   collectTextures(pid);

   int replaced = 0;
   for (int i = 0; i < collectedCount; i++) {
      TexEntry *tex = &collected[i];
      if (tex->location != 0) continue;

      int state;
      int current = matchLiveTexture(pid, tex, &state);   // matches the original or any of our replacements
      if (current < 0) continue;
      uint32_t origHash = pack[current].origHash;

      char snapPath[224];
      snprintf(snapPath, sizeof snapPath, "%s/%08x.bin", snapDir, origHash);
      int want = getWinningEntry(origHash, partOn);

      // no part wins: the original should show. restore it if a replacement is live, then drop the snapshot.
      if (want < 0) {
         if (state == TEX_REPLACED) {
            char snapFile[24];
            snprintf(snapFile, sizeof snapFile, "%08x.bin", origHash);
            if (writeReplacement(pid, tex->offset, tex->size, snapDir, snapFile) == 0) removeFilePath(snapPath);
         }
         continue;
      }

      if (state == TEX_REPLACED && current == want) { replaced++; continue; }   // winner already live
      if (state == TEX_ORIGINAL && !fileExists(snapPath)) dumpOneTexture(pid, tex, snapPath);   // snapshot once
      if (writeReplacement(pid, tex->offset, tex->size, packDir, pack[want].file) == 0) {
         replaced++;
         logInfo(TAG "rebuild: %ux%u -> %s\n", tex->width, tex->height, pack[want].file);
      }
   }
   logInfo(TAG "rebuild done: %s =%d\n", patchName, replaced);
   return replaced;
}

// list a patch's parts for the drill-in menu. returns 0 for a whole (partless) patch.
int getPatchParts(const char *titleId, const char *patchName, PatchPart *out, int maxParts)
{
   char packDir[192];
   snprintf(packDir, sizeof packDir, "%s/%s/%s", PATCHES_ROOT, titleId, patchName);
   loadPack(packDir);

   int n = partCount < maxParts ? partCount : maxParts;
   for (int i = 0; i < n; i++) {
      int k = 0;
      while (parts[i].name[k] && k < (int)sizeof out[i].name - 1) { out[i].name[k] = parts[i].name[k]; k++; }
      out[i].name[k] = 0;
      out[i].group = parts[i].group;
      out[i].pickOne = parts[i].group >= 0 ? groups[parts[i].group].pickOne : 0;
   }
   return n;
}
