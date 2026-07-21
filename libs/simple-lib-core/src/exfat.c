// exfat.c - minimal exFAT reader/writer for the PS3 file manager. See exfat.h.
//
// Hand-written from the Microsoft exFAT specification; the directory entry-set
// layout, timestamps and cluster math were cross-checked against ChaN's FatFs
// (the lib this replaced). The lv2 storage syscall numbers, argument order and
// device-info struct are taken verbatim from the in-tree references
// (apps/ManaGunZ/payloads/rawseciso/storage.h, apps/xai_plugin functions.cpp):
//   600 open, 601 close, 602 read, 609 get_device_info.
// No libc: storage I/O goes through simple-lib-core's scCall trampolines and the
// string helpers (memCopy / memSet / utf16ToUtf8 / strCmpICase), so this links
// into prx plugins too.

#include "exfat.h"
#include "vfs.h"              // VfsOps + probe/release backend registration
#include "storage-device.h"   // device ids, info + raw sector I/O (shared lv2 storage layer)
#include "syscall.h"          // scCall1/2/4/7
#include "thread.h"           // sys_lwmutex helpers (lock/unlock)
#include "string-utilities.h" // memCopy, memSet, utf16ToUtf8, strCmpICase
#include <sys/timer.h>        // sys_timer_usleep
#include <cell/rtc.h>         // cellRtcGetCurrentClock (real entry timestamps in UTC; needs RTC loaded)

#define STORAGE_WRITE      603   // the write path is exFAT-only; open/close/read come from storage-device.h

#define STORAGE_BUSY       0x80010002u   // lv2 "device not ready" (settling / ejected)
#define SYSIO_RETRY        8
#define SYSIO_RETRY_US     50000
#define SYSIO_SETTLE_US    62500         // settle gap after open, before the first read

#define STORAGE_ALIGN      32            // lv2 storage DMA buffer alignment
#define EXFAT_MAX_SECTOR   4096          // largest sector we support
#define EXFAT_READ_BOUNCE  32768         // file-read bounce: up to this many bytes
                                         // (one ~32 KB cluster) per sys_storage_read,
                                         // instead of one 512-byte sector at a time

#define DIR_ENTRY_BYTES    32
#define MAX_SET_ENTRIES    19            // File + Stream + ceil(255/15)=17 Name entries
#define ENTRY_END          0x00          // end of directory
#define ENTRY_BITMAP       0x81          // allocation bitmap
#define ENTRY_FILE         0x85          // file/dir entry (in-use)
#define ENTRY_STREAM       0xC0          // stream extension
#define ENTRY_NAME         0xC1          // file name fragment
#define ATTR_DIRECTORY     0x10
#define FLAG_NO_FAT_CHAIN  0x02          // GeneralSecondaryFlags: contiguous data
#define EXFAT_EOC          0xFFFFFFFFu

#define MBR_PART_TABLE     446
#define MBR_PART_ENTRIES   4
#define MBR_PART_SIZE      16
#define MBR_TYPE_GPT       0xEE   // protective-MBR partition type that marks a GPT disk
#define GPT_HEADER_LBA     1      // the GPT header occupies the second sector
#define GPT_MAX_ENTRIES    128    // cap on the partition-entry scan (the GPT default)
#define GPT_ENTRY_MIN_SIZE 128    // smallest valid GPT partition-entry size
// Fallback ceiling for partition-table LBAs when the device size is unknown (getStorageInfo failed,
// deviceSectors == 0): 2^36 sectors is >=32 TB at 512-byte sectors - past any real removable medium,
// yet finite, so a hostile partition entry can't steer a scan read to an arbitrary 64-bit sector.
#define EXFAT_SCAN_LBA_CAP (1ull << 36)

// device ids, info and raw sector I/O live in storage-device.h (the layer shared with the VFS
// and the disc dumper). The write path below stays here - it's the exFAT backend's.

// little-endian readers (exFAT is LE on disk, the PPU is big-endian).
static uint16_t readLe16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t readLe32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t readLe64(const uint8_t *p) { return (uint64_t)readLe32(p) | ((uint64_t)readLe32(p + 4) << 32); }

// little-endian writers (mirror readLe16/readLe32/readLe64 for the write path).
static void writeLe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void writeLe32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void writeLe64(uint8_t *p, uint64_t v) { writeLe32(p, (uint32_t)v); writeLe32(p + 4, (uint32_t)(v >> 32)); }

// Shared, 32-byte-aligned scratch. Only touched while the (single) caller holds
// the exFAT backend's lock, so these are safe to share across volumes/handles.
// Per-mount epoch: an id assigned to each successful mount, used as the shared-cache key instead of
// the lv2 storageHandle (which lv2 RECYCLES after closeStorage, so a reused handle could otherwise
// validate a stale cache entry against a different volume). Each mount gets an id no earlier mount
// had, so a cache entry can never match a later mount even if a reset were missed. 0 = "none" sentinel.
static uint32_t mountEpoch;          // last epoch handed out (first mount gets 1; wraps skip 0)

// All large scratch lives in ONE heap block allocated on first mount and freed when the last volume
// unmounts (ensureScratch / releaseScratchIfIdle), so an idle system with no USB inserted holds none of
// it. Pointers below carve that block; they are valid only while a volume is mounted. mount holds
// exfatLock so these shared buffers are safe, and keeping them off the stack lets the hotplug poll
// thread run on a small stack.
static uint8_t *dirSector;       // cached directory sector (lba/epoch below as before)
static uint64_t dirSectorLba = ~0ULL;
static uint32_t dirSectorEpoch = 0;
static uint8_t *fatSector;       // cached FAT sector
static uint64_t fatSectorLba = ~0ULL;
static uint32_t fatSectorEpoch = 0;
static uint8_t *fileSector;      // single-sector scratch for getExfatFree + ~32 KB read bounce
static uint8_t *writeScratch;    // read-modify-write scratch for bitmap / FAT / directory sectors
static uint8_t *bootSector;      // boot-sector scratch for mountExfat

// Up-case casefold table, sparse: only the code points that differ from identity are stored (a standard
// exFAT $UpCase has ~900 of 65536), as parallel sorted-by-code-point arrays. exFAT casefold and NameHash
// map through upcaseOf() instead of re-reading the on-disk table on every create / rename / lookup.
// Reset on mount and unmount, not by per-operation cache invalidation, so a burst of writes keeps it warm.
#define EXFAT_UPCASE_MAX 2048    // headroom over the ~900 non-identity entries a standard $UpCase defines
static uint16_t *upcaseCp;       // code points whose up-cased form differs (ascending)
static uint16_t *upcaseUp;       // their up-cased forms (parallel to upcaseCp)
static uint32_t  upcaseCount;
static uint32_t  upcaseTableEpoch = 0;

// One 64 KB-page heap block holds every buffer above. lv2 rejects a SYS_PAGE_64K request whose size is
// not a 64 KB multiple, so round the ~56 KB the buffers need up to a single 64 KB page (which is also the
// smallest page sys_memory_allocate offers). Costs one page while any volume is mounted, nothing otherwise.
#define EXFAT_SCRATCH_USED  (4u * EXFAT_MAX_SECTOR + EXFAT_READ_BOUNCE + 4u * EXFAT_UPCASE_MAX)
#define EXFAT_SCRATCH_BYTES ((EXFAT_SCRATCH_USED + 0xFFFFu) & ~0xFFFFu)
// keep the working set inside a single 64 KB page: growing it past one page would break the VSH budget
typedef char exfatScratchFitsOnePage[(EXFAT_SCRATCH_BYTES <= 0x10000u) ? 1 : -1];
static uint32_t scratchAddr;     // sysMemAllocate handle (0 = not allocated)
static void releaseScratchIfIdle(void);   // defined after the volume pool

// Allocates and carves the shared scratch block if it isn't already. Returns 0 / -1.
static int ensureScratch(void)
{
   if (scratchAddr) return 0;
   uint32_t addr = 0;
   if (sysMemAllocate(EXFAT_SCRATCH_BYTES, SYS_PAGE_64K, &addr) != 0 || !addr) return -1;
   uint8_t *base = (uint8_t *)(uintptr_t)addr;
   dirSector    = base;
   fatSector    = base + 1u * EXFAT_MAX_SECTOR;
   writeScratch = base + 2u * EXFAT_MAX_SECTOR;
   bootSector   = base + 3u * EXFAT_MAX_SECTOR;
   fileSector   = base + 4u * EXFAT_MAX_SECTOR;
   uint8_t *up  = fileSector + EXFAT_READ_BOUNCE;       // EXFAT_MAX_SECTOR is a multiple of 32: u16-aligned
   upcaseCp     = (uint16_t *)up;
   upcaseUp     = (uint16_t *)(up + 2u * EXFAT_UPCASE_MAX);
   upcaseCount  = 0;
   dirSectorLba = fatSectorLba = ~0ULL;                 // the carved caches start empty
   scratchAddr  = addr;
   return 0;
}

// End-of-directory cache: the 0x00 marker position of recently-appended-to directories, so a burst of
// inserts into one directory (e.g. pasting a folder) doesn't rescan it from the start each time, which
// is O(files^2). A small N-way set (not one slot) so a paste that interleaves inserts across several
// directories keeps each one's marker cached instead of evicting on every switch. Keyed by (mount
// epoch, directory first cluster); validated on use (the slot must still read 0x00) and dropped
// whenever a cluster is freed, so a placement can never aim at reused space. Only under the backend lock.
#define DIR_END_SLOTS 4
static struct {
   uint32_t      epoch;     // mount epoch this entry belongs to (0 = empty slot)
   uint32_t      cluster;   // that directory's first cluster (cache key)
   ExfatEntryLoc loc;       // location of its 0x00 end-of-directory marker
} dirEndCache[DIR_END_SLOTS];
static uint32_t dirEndVictim;   // round-robin eviction index

static void invalidateDirEnd(void) { for (int i = 0; i < DIR_END_SLOTS; i++) dirEndCache[i].epoch = 0; }

// Returns the cached end marker for `dirCluster` on this mount, or 0 if not cached.
static const ExfatEntryLoc *findDirEnd(uint32_t epoch, uint32_t dirCluster)
{
   for (int i = 0; i < DIR_END_SLOTS; i++)
      if (dirEndCache[i].epoch == epoch && dirEndCache[i].cluster == dirCluster) return &dirEndCache[i].loc;
   return 0;
}

// Caches `loc` as the end marker for `dirCluster`, reusing its slot or evicting round-robin.
static void storeDirEnd(uint32_t epoch, uint32_t dirCluster, const ExfatEntryLoc *loc)
{
   for (int i = 0; i < DIR_END_SLOTS; i++)
      if (dirEndCache[i].epoch == epoch && dirEndCache[i].cluster == dirCluster) { dirEndCache[i].loc = *loc; return; }
   uint32_t slot = dirEndVictim++ % DIR_END_SLOTS;
   dirEndCache[slot].epoch = epoch; dirEndCache[slot].cluster = dirCluster; dirEndCache[slot].loc = *loc;
}

static void invalidateCaches(void)
{
   dirSectorLba = ~0ULL;
   dirSectorEpoch = 0;
   fatSectorLba = ~0ULL;
   fatSectorEpoch = 0;
}

// Reads `count` sectors at `lba` into a 32-byte-aligned buffer, retrying while the
// device reports "not ready" (hotplug settling). Every caller passes an aligned
// static buffer or aligned local, which is what lv2 storage DMA requires.
// Returns 0 on success, -1 on a hard error.
static int readSectors(int storageHandle, uint64_t lba, uint32_t count, void *aligned)
{
   for (int attempt = 0; attempt < SYSIO_RETRY; attempt++) {
      uint32_t got = 0;
      int rc = readStorageRaw(storageHandle, lba, count, aligned, &got);
      if (rc == 0 && got == count) return 0;
      if ((uint32_t)rc != STORAGE_BUSY) return -1;
      sys_timer_usleep(SYSIO_RETRY_US);
   }
   return -1;
}

static int writeStorageRaw(int storageHandle, uint64_t sector, uint32_t count,
                           const void *buffer, uint32_t *outWritten)
{
   // 603 sys_storage_write: same arg order as the 602 read (storageHandle, 0, sector, count,
   // buf, &written, flags). Verified against apps/xai_plugin functions.cpp and
   // apps/webMAN-MOD VshFpsCounter SystemCalls.cpp. flags 0 for plain storage.
   return (int)scCall7(STORAGE_WRITE, (uint64_t)storageHandle, 0, sector, count,
                       (uint64_t)(uintptr_t)buffer, (uint64_t)(uintptr_t)outWritten, 0);
}

// Writes `count` sectors at `lba` from a 32-byte-aligned buffer, retrying while
// the device reports "not ready". Returns 0 on success, -1 on a hard error.
static int writeSectors(int storageHandle, uint64_t lba, uint32_t count, const void *aligned)
{
   for (int attempt = 0; attempt < SYSIO_RETRY; attempt++) {
      uint32_t put = 0;
      int rc = writeStorageRaw(storageHandle, lba, count, aligned, &put);
      if (rc == 0 && put == count) return 0;
      if ((uint32_t)rc != STORAGE_BUSY) return -1;
      sys_timer_usleep(SYSIO_RETRY_US);
   }
   return -1;
}

static uint32_t getClusterBytes(const ExfatVolume *vol)
{
   return vol->bytesPerSector * vol->sectorsPerCluster;
}

// Sets (dirty!=0) or clears the VolumeDirty bit in VolumeFlags (offset 106) of the main and
// backup VBR. That field sits in the boot-region's checksum-EXCLUDED range, so flipping it keeps
// the boot checksum valid - no recompute needed. Marking the volume dirty on the first write lets
// a host (chkdsk) know to verify it after a crash; a clean unmount clears it. Returns 0 / -1.
#define EXFAT_FLAG_VOLUME_DIRTY 0x0002
static int setVolumeDirty(ExfatVolume *vol, int dirty)
{
   for (int copy = 0; copy < 2; copy++) {     // main VBR at +0, backup boot region at +12
      uint64_t lba = vol->partitionOffset + (copy ? 12 : 0);
      if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
      uint16_t flags = readLe16(writeScratch + 106);
      flags = dirty ? (uint16_t)(flags | EXFAT_FLAG_VOLUME_DIRTY)
                    : (uint16_t)(flags & ~EXFAT_FLAG_VOLUME_DIRTY);
      writeLe16(writeScratch + 106, flags);
      if (writeSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
   }
   invalidateCaches();
   return 0;
}

// Marks the volume dirty once per mount, before its first mutation. Best-effort: if the flag
// can't be written the mutation still proceeds (the flag is a recovery hint, not a gate).
static void ensureVolumeDirty(ExfatVolume *vol)
{
   if (!vol->volumeDirty && setVolumeDirty(vol, 1) == 0) vol->volumeDirty = 1;
}

// first absolute sector (LBA) of a data cluster (clusters are numbered from 2).
static uint64_t clusterToSector(const ExfatVolume *vol, uint32_t cluster)
{
   return vol->partitionOffset + (uint64_t)vol->clusterHeapOffset
        + (uint64_t)(cluster - 2) * vol->sectorsPerCluster;
}

static int isClusterValid(const ExfatVolume *vol, uint32_t cluster)
{
   // <= 0xFFFFFFF6 keeps the bad-cluster (0xFFFFFFF7) and EOC markers out of the valid range
   // explicitly, so a FAT entry holding one is never followed as a real cluster.
   return cluster >= 2 && cluster <= 0xFFFFFFF6u && (cluster - 2) < vol->clusterCount;
}

// LBA of the FAT sector holding `cluster`'s 32-bit entry, with its byte offset within.
static uint64_t getFatEntryLba(const ExfatVolume *vol, uint32_t cluster, uint32_t *within)
{
   uint64_t byteOffset = (uint64_t)cluster * 4;
   *within = (uint32_t)(byteOffset % vol->bytesPerSector);
   return vol->partitionOffset + (uint64_t)vol->fatOffset + byteOffset / vol->bytesPerSector;
}

// Ensures the shared FAT read-cache (fatSector) holds the sector at `lba`. Returns 0 / -1.
static int loadFatSector(const ExfatVolume *vol, uint64_t lba)
{
   if (lba != fatSectorLba || vol->cacheEpoch != fatSectorEpoch) {
      if (readSectors(vol->storageHandle, lba, 1, fatSector) != 0) return -1;
      fatSectorLba = lba;
      fatSectorEpoch  = vol->cacheEpoch;
   }
   return 0;
}

// Follows the FAT one link. Returns the next cluster, or 0 at end-of-chain.
static uint32_t getNextCluster(const ExfatVolume *vol, uint32_t cluster)
{
   uint32_t within;
   uint64_t lba = getFatEntryLba(vol, cluster, &within);
   if (loadFatSector(vol, lba) != 0) return 0;
   uint32_t next = readLe32(fatSector + within);
   return (next == EXFAT_EOC || !isClusterValid(vol, next)) ? 0 : next;
}

// Writes one FAT entry through the coherent FAT cache (one read + one write per sector
// touched, reused across a run of same-sector updates). Returns 0 / -1.
static int setFat(ExfatVolume *vol, uint32_t cluster, uint32_t value)
{
   uint32_t within;
   uint64_t lba = getFatEntryLba(vol, cluster, &within);
   if (loadFatSector(vol, lba) != 0) return -1;
   writeLe32(fatSector + within, value);
   return writeSectors(vol->storageHandle, lba, 1, fatSector);
}

// Appends `next` to the chain after `prev`: sets prev->next and next->EOC. When both
// entries share a FAT sector (the usual sequential case) it writes that sector once
// instead of twice. EOC is written no later than the link, so a crash leaves `next` an
// orphan (a recoverable bitmap leak) rather than a chain into an unterminated cluster.
static int chainCluster(ExfatVolume *vol, uint32_t prev, uint32_t next)
{
   uint32_t prevWithin, nextWithin;
   uint64_t prevLba = getFatEntryLba(vol, prev, &prevWithin);
   uint64_t nextLba = getFatEntryLba(vol, next, &nextWithin);

   if (prevLba == nextLba) {
      if (loadFatSector(vol, prevLba) != 0) return -1;
      writeLe32(fatSector + nextWithin, EXFAT_EOC);
      writeLe32(fatSector + prevWithin, next);
      return writeSectors(vol->storageHandle, prevLba, 1, fatSector);
   }
   if (setFat(vol, next, EXFAT_EOC) != 0) return -1;
   return setFat(vol, prev, next);
}

// Converts an exFAT timestamp to unix seconds (UTC). exFAT stores LOCAL time plus a per-field
// UtcOffset byte (bit 7 = valid, bits 0-6 = signed offset in 15-minute units); `tzoffset` is that
// byte, applied here to normalize the local time to UTC. A byte of 0 (no offset recorded) leaves
// the value as-is, matching how other readers treat an unanchored timestamp.
static uint64_t timestampToUnix(uint32_t timestamp, uint8_t tzoffset)
{
   uint32_t sec  = (timestamp & 0x1F) * 2;
   uint32_t min  = (timestamp >> 5) & 0x3F;
   uint32_t hour = (timestamp >> 11) & 0x1F;
   uint32_t day  = (timestamp >> 16) & 0x1F;
   uint32_t mon  = (timestamp >> 21) & 0x0F;
   uint32_t year = 1980 + ((timestamp >> 25) & 0x7F);
   if (mon < 1)  mon = 1;
   if (mon > 12) mon = 12;   // a corrupt/foreign month (the field holds 0-15) must not feed garbage
   if (day < 1)  day = 1;    // into the civil-days math below
   if (day > 31) day = 31;

   // days from civil (Howard Hinnant's algorithm), epoch 1970-01-01.
   int y = (int)year - (mon <= 2);
   int era = (y >= 0 ? y : y - 399) / 400;
   uint32_t yoe = (uint32_t)(y - era * 400);
   uint32_t doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
   uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   long days = (long)era * 146097 + (long)doe - 719468;
   int64_t unixTime = (int64_t)((uint64_t)days * 86400 + hour * 3600 + min * 60 + sec);

   if (tzoffset & 0x80) {                              // OffsetValid: convert local -> UTC
      int offsetQuarters = (int)(int8_t)(tzoffset << 1) / 2;   // sign-extend the 7-bit field
      unixTime -= (int64_t)offsetQuarters * 15 * 60;
   }
   return (uint64_t)unixTime;
}

// true if buffer holds an exFAT boot sector ("EXFAT   " at offset 3, sig 0xAA55).
static int hasExfatBoot(const uint8_t *boot)
{
   static const char tag[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
   for (int i = 0; i < 8; i++) {
      if (boot[3 + i] != (uint8_t)tag[i]) return 0;
   }
   return boot[510] == 0x55 && boot[511] == 0xAA;
}

// true if a sector begins with the GPT header signature "EFI PART".
static int hasGptHeader(const uint8_t *sector)
{
   static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
   for (int i = 0; i < 8; i++) {
      if (sector[i] != (uint8_t)sig[i]) return 0;
   }
   return 1;
}

// If sector `lba` holds an exFAT boot sector, copies it into `boot`, records the start in
// *volStart and returns 1; otherwise 0. Reads through the caller's aligned `vbr` scratch.
static int tryExfatVbr(int storageHandle, uint64_t lba, uint32_t sectorBytes, uint64_t deviceSectors,
                       uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   // Reject an out-of-range starting LBA (an attacker-controlled partition field) BEFORE reading it,
   // so a crafted table can't steer a read to a wild sector. When the device size is unknown
   // (deviceSectors == 0) fall back to a finite ceiling instead of trusting the field unbounded.
   uint64_t sectorBound = deviceSectors ? deviceSectors : EXFAT_SCAN_LBA_CAP;
   if (lba == 0 || lba >= sectorBound) return 0;
   if (readSectors(storageHandle, lba, 1, vbr) != 0 || !hasExfatBoot(vbr)) return 0;
   memCopy(boot, vbr, (int)sectorBytes);
   *volStart = lba;
   return 1;
}

// Walks a GPT partition table (the disk is GPT when its protective-MBR entry is type 0xEE) and
// returns the first partition whose first sector is an exFAT VBR. `scratch` reads the GPT header
// and entry array; `vbr` reads partition candidates (a separate buffer so the entry array it is
// iterating isn't clobbered). Returns 1 on success (boot/volStart set), 0 otherwise.
static int locateExfatInGpt(int storageHandle, uint32_t sectorBytes, uint64_t deviceSectors,
                            uint8_t *scratch, uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   if (readSectors(storageHandle, GPT_HEADER_LBA, 1, scratch) != 0 || !hasGptHeader(scratch)) return 0;
   uint64_t entriesLba = readLe64(scratch + 72);   // PartitionEntryLBA
   uint32_t entryCount = readLe32(scratch + 80);   // NumberOfPartitionEntries
   uint32_t entrySize  = readLe32(scratch + 84);   // SizeOfPartitionEntry
   if (entrySize < GPT_ENTRY_MIN_SIZE || entrySize > sectorBytes) return 0;
   uint64_t sectorBound = deviceSectors ? deviceSectors : EXFAT_SCAN_LBA_CAP;   // finite ceiling when size unknown
   if (entriesLba >= sectorBound) return 0;   // entry-array LBA from a hostile header (or wild when size unknown)
   if (entryCount > GPT_MAX_ENTRIES) entryCount = GPT_MAX_ENTRIES;
   uint32_t perSector = sectorBytes / entrySize;

   for (uint32_t i = 0; i < entryCount; i++) {
      if (i % perSector == 0 && readSectors(storageHandle, entriesLba + i / perSector, 1, scratch) != 0) return 0;
      const uint8_t *entry = scratch + (i % perSector) * entrySize;
      int used = 0;
      for (int k = 0; k < 16; k++) if (entry[k]) { used = 1; break; }   // non-zero PartitionTypeGUID
      // entry+32 = StartingLBA
      if (used && tryExfatVbr(storageHandle, readLe64(entry + 32), sectorBytes, deviceSectors, vbr, boot, volStart)) return 1;
   }
   return 0;
}

// Locates the exFAT volume reachable through `storageHandle`: a superfloppy at LBA 0, or a partition listed
// in an MBR or GPT table. On success `boot` holds the volume's VBR and *volStart its start LBA.
// `scratch` and `vbr` are caller-owned aligned one-sector buffers. Returns 1 / 0.
static int locateExfatVolume(int storageHandle, uint8_t *boot, uint8_t *scratch, uint8_t *vbr,
                             uint32_t sectorBytes, uint64_t deviceSectors, uint64_t *volStart)
{
   if (hasExfatBoot(boot)) { *volStart = 0; return 1; }              // superfloppy (volume at LBA 0)
   if (boot[510] != 0x55 || boot[511] != 0xAA) return 0;            // neither exFAT nor partitioned

   for (int i = 0; i < MBR_PART_ENTRIES; i++) {
      const uint8_t *part = boot + MBR_PART_TABLE + i * MBR_PART_SIZE;
      uint8_t type = part[4];
      if (type == 0) continue;                                       // unused entry
      if (type == MBR_TYPE_GPT) {                                    // GPT disk: walk its table
         if (locateExfatInGpt(storageHandle, sectorBytes, deviceSectors, scratch, vbr, boot, volStart)) return 1;
         continue;
      }
      // part+8 = MBR partition StartingLBA
      if (tryExfatVbr(storageHandle, readLe32(part + 8), sectorBytes, deviceSectors, vbr, boot, volStart)) return 1;
   }
   return 0;
}

// Scans the root directory for the allocation-bitmap (0x81), up-case (0x82) and
// volume-label (0x83) entries and records them on the volume. Defined below.
static void loadVolumeMeta(ExfatVolume *vol);

// Counts free clusters by scanning the whole allocation bitmap (used once at mount to seed the
// running free-cluster total). Returns 0 / -1. Defined below.
static int countFreeClustersOnDisk(const ExfatVolume *vol, uint32_t *outFree);

// Validates the (untrusted, removable-media) VBR geometry before any of it is used to compute
// LBAs. The shifts must be in spec range (no undefined-behaviour shift, no absurd cluster size),
// NumberOfFats must be 1 or 2, and the FAT/heap layout, cluster count and root cluster must all
// fit the device. Returns 1 if the geometry is usable, 0 to reject the volume. `deviceSectors`
// of 0 means the device size is unknown, so the size bound is skipped.
static int validExfatGeometry(const uint8_t *boot, uint32_t deviceSectorSize,
                              uint64_t deviceSectors, uint64_t volStart)
{
   uint8_t bytesPerSectorShift    = boot[108];   // BytesPerSectorShift
   uint8_t sectorsPerClusterShift = boot[109];   // SectorsPerClusterShift
   uint8_t numberOfFats           = boot[110];   // NumberOfFats

   if (bytesPerSectorShift < 9 || bytesPerSectorShift > 12) return 0;       // 512..4096 bytes
   if (sectorsPerClusterShift > 25 - bytesPerSectorShift) return 0;         // cluster <= 32 MB, no shift UB
   if ((1u << bytesPerSectorShift) != deviceSectorSize) return 0;           // must match the device
   // Reject NumberOfFats == 2 (TexFAT): this driver reads/writes only the first FAT and does not
   // consult VolumeFlags.ActiveFat, so on a 2-FAT volume the second FAT would silently go stale (or,
   // with ActiveFat == 1, the active FAT would be ignored entirely). A 1-FAT volume can't have that
   // ambiguity. Almost all removable media is formatted with a single FAT.
   if (numberOfFats != 1) return 0;

   uint32_t bytesPerSector    = 1u << bytesPerSectorShift;
   uint32_t sectorsPerCluster = 1u << sectorsPerClusterShift;
   uint32_t fatOffset         = readLe32(boot + 80);
   uint32_t fatLength         = readLe32(boot + 84);
   uint32_t clusterHeapOffset = readLe32(boot + 88);
   uint32_t clusterCount      = readLe32(boot + 92);
   uint32_t rootCluster       = readLe32(boot + 96);

   if (clusterCount > 0xFFFFFFF5u) return 0;                                // exFAT maximum cluster count
   if (rootCluster < 2 || (rootCluster - 2) >= clusterCount) return 0;      // root must be a real data cluster
   if (fatOffset < 24 || clusterHeapOffset < fatOffset) return 0;   // 24-sector boot region, then FAT, then heap

   // The FAT must physically hold every cluster's 4-byte entry (entries 0,1 reserved + clusterCount)
   // and must fit between FatOffset and the cluster heap. Without this, a crafted/oversized FatLength
   // (or a heap that overlaps the FAT) would let a FAT entry LBA land inside the data heap, so a
   // write while chaining clusters could corrupt file data. (FatLength was previously unvalidated.)
   uint64_t fatBytesNeeded = ((uint64_t)clusterCount + 2) * 4;
   uint64_t fatSectorsNeeded = (fatBytesNeeded + bytesPerSector - 1) / bytesPerSector;
   if (fatLength < fatSectorsNeeded) return 0;                              // FAT too small to map every cluster
   if ((uint64_t)fatOffset + fatLength > clusterHeapOffset) return 0;       // FAT must end before the heap

   uint64_t heapEnd = (uint64_t)clusterHeapOffset + (uint64_t)clusterCount * sectorsPerCluster;
   if (heapEnd < clusterHeapOffset) return 0;                               // overflow
   // Bound the heap by the VBR's self-declared VolumeLength too, so a hostile ClusterHeapOffset near
   // 2^32 is rejected even when the device size is unknown (deviceSectors == 0) and the check below is
   // skipped - otherwise a cluster->LBA could resolve to a wild (but in-range) sector on such media.
   uint64_t volumeLength = readLe64(boot + 72);                            // VolumeLength, in sectors
   if (volumeLength != 0 && heapEnd > volumeLength) return 0;               // heap must fit the declared volume
   if (deviceSectors != 0 && volStart + heapEnd > deviceSectors) return 0;  // heap must fit the device
   return 1;
}

int mountExfat(ExfatVolume *vol, int drive)
{
   memSet(vol, 0, (int)sizeof(*vol));
   invalidateDirEnd();   // a fresh volume on this storageHandle must not match a stale end-cache

   uint64_t deviceId = getUsbDeviceId(drive);
   StorageDeviceInfo info;
   int      haveInfo         = (getStorageInfo(deviceId, &info) == 0);
   uint32_t deviceSectorSize = haveInfo ? info.sectorSize  : 512;
   uint64_t deviceSectors    = haveInfo ? info.sectorCount : 0;   // 0 = unknown (skip the device-size bound)
   if (deviceSectorSize == 0 || deviceSectorSize > EXFAT_MAX_SECTOR) return EXFAT_MOUNT_NOT_READY;

   int storageHandle;
   if (openStorage(deviceId, &storageHandle) < 0) return EXFAT_MOUNT_NOT_READY;
   // lv2 storage faults if the first read lands too soon after open; settle like the
   // reference stacks (libntfs ps3_io.c, IRISMAN) do, not via log-induced delays.
   sys_timer_usleep(SYSIO_SETTLE_US);

   // bring up the shared scratch (needed from the first read below); release it again on any failure
   if (ensureScratch() != 0) { closeStorage(storageHandle); return EXFAT_MOUNT_NOT_READY; }

   // Read LBA 0 and locate the exFAT volume: superfloppy, or an MBR- or GPT-partitioned disk.
   // The scan needs two scratch sectors. Rather than spend 8 KB of stack (this can run on a size-
   // sensitive 16 KB plugin thread), reuse the operational sector caches AS generic scratch: the
   // mount holds exfatLock so nothing else touches them, and invalidateCaches() below drops
   // whatever the scan leaves before the caches are first used for real. Aliased so the scan reads
   // as plain scratch, not as the dir/FAT caches.
   uint8_t *scanScratch = dirSector;   // partition table / GPT-entry sectors
   uint8_t *scanVbr     = fatSector;   // candidate boot-record sectors
   uint8_t *boot = bootSector;   // off the stack (see bootSector) so the mount path fits a small stack
   if (readSectors(storageHandle, 0, 1, boot) != 0) {
      closeStorage(storageHandle);
      releaseScratchIfIdle();
      return EXFAT_MOUNT_NOT_READY;
   }
   uint64_t volStart = 0;
   if (!locateExfatVolume(storageHandle, boot, scanScratch, scanVbr, deviceSectorSize, deviceSectors, &volStart)) {
      closeStorage(storageHandle);                       // read OK but no exFAT volume here (e.g. FAT32/NTFS)
      releaseScratchIfIdle();
      return EXFAT_MOUNT_NOT_EXFAT;
   }

   // Validate the geometry from the (untrusted) VBR before trusting any field that feeds a
   // cluster->LBA computation. A hostile/malformed exFAT image is rejected, not acted on.
   if (!validExfatGeometry(boot, deviceSectorSize, deviceSectors, volStart)) {
      closeStorage(storageHandle);
      releaseScratchIfIdle();
      return EXFAT_MOUNT_NOT_EXFAT;
   }

   vol->storageHandle                = storageHandle;
   vol->cacheEpoch        = (mountEpoch + 1) ? ++mountEpoch : (mountEpoch = 1);   // fresh cache key; skip the 0 sentinel on wrap
   vol->drive             = (uint8_t)drive;
   vol->deviceSectorSize  = deviceSectorSize;
   vol->bytesPerSector    = 1u << boot[108];    // BytesPerSectorShift
   vol->sectorsPerCluster = 1u << boot[109];    // SectorsPerClusterShift
   vol->partitionOffset   = volStart;
   vol->fatOffset         = readLe32(boot + 80);    // FatOffset
   vol->clusterHeapOffset = readLe32(boot + 88);    // ClusterHeapOffset
   vol->clusterCount      = readLe32(boot + 92);    // ClusterCount
   vol->rootCluster       = readLe32(boot + 96);    // FirstClusterOfRootDirectory
   vol->mounted           = 1;
   invalidateCaches();
   upcaseTableEpoch = 0;    // fresh volume: drop any cached up-case table
   loadVolumeMeta(vol);   // bitmap + up-case table (for writes) and the volume label
   // Seed the running free-cluster count once (one bitmap scan) so getExfatFree is O(1) thereafter;
   // markClusterRun keeps it in sync on every alloc/free. 0 if the bitmap is missing/unreadable.
   uint32_t seedFree = 0;
   vol->freeClusters = (vol->bitmapCluster != 0 && countFreeClustersOnDisk(vol, &seedFree) == 0) ? seedFree : 0;
   return EXFAT_MOUNT_OK;
}

void unmountExfat(ExfatVolume *vol)
{
   if (!vol->mounted) return;
   // Clear the on-disk dirty flag for the host, but only if the device is still physically present.
   // On a hotplug eject the device is already gone, so the read-modify-write would just stall on
   // STORAGE_BUSY retries (and could land on a recycled storage handle); skip it and leave the flag
   // set - the safe direction, the host re-verifies on next mount.
   if (vol->volumeDirty && isUsbDevicePresent(vol->drive)) setVolumeDirty(vol, 0);
   closeStorage(vol->storageHandle);
   vol->mounted = 0;
   invalidateCaches();   // drop the sector caches keyed by the old epoch
   invalidateDirEnd();   // and the end-of-directory cache
   upcaseTableEpoch = 0;   // and the up-case table cache
   releaseScratchIfIdle();   // last volume out frees the shared scratch block
}

void openExfatDir(ExfatDir *dir, const ExfatVolume *vol, uint32_t firstCluster, int noFatChain, uint64_t byteLength)
{
   dir->vol            = vol;
   dir->cluster        = firstCluster;
   dir->sectorInClu    = 0;
   dir->entryInSector  = 0;
   dir->noFatChain     = noFatChain;
   dir->clustersWalked = 0;
   dir->ioError        = 0;
   // Bound the walk by the directory's own DataLength so a malformed entry can't make us read
   // past the allocation (a contiguous/NoFatChain directory would otherwise spill into whatever
   // physically follows it and parse foreign clusters as entries). 0 = unbounded (the root, whose
   // size no entry records; its FAT chain is bounded by the clusterCount cycle guard instead).
   if (byteLength == 0) {
      dir->clusterLimit = 0;
   } else {
      uint32_t clusterBytes = getClusterBytes(vol);
      uint64_t clusters = (byteLength + clusterBytes - 1) / clusterBytes;
      dir->clusterLimit = clusters > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)clusters;
   }
}

void closeExfatDir(ExfatDir *dir)
{
   dir->cluster = 0;
   dir->vol     = 0;
}

// advances the directory chain to the next cluster (0 when it ends). Bounds the walk by the
// volume's cluster count so a cyclic/corrupt directory FAT ends the listing instead of spinning.
static uint32_t advanceDirCluster(ExfatDir *dir)
{
   if (++dir->clustersWalked > dir->vol->clusterCount) return 0;            // cycle guard
   if (dir->clusterLimit != 0 && dir->clustersWalked >= dir->clusterLimit) return 0;   // past DataLength
   if (dir->noFatChain) {
      uint32_t next = dir->cluster + 1;
      return isClusterValid(dir->vol, next) ? next : 0;
   }
   return getNextCluster(dir->vol, dir->cluster);
}

// returns the next 32-byte directory entry (into dirSector), or 0 at chain end.
static const uint8_t *getNextEntry(ExfatDir *dir)
{
   const ExfatVolume *vol = dir->vol;
   // vol is NULL once the device was yanked while this dir was open (detachVolumeHandles),
   // so test it BEFORE dereferencing mounted - a late read on a stale handle must end the
   // walk, not crash (mirrors readExfat).
   if (!vol || !vol->mounted) return 0;   // ejected mid-walk: end the dir, never touch the closed storageHandle
   if (!isClusterValid(vol, dir->cluster)) return 0;

   uint64_t lba = clusterToSector(vol, dir->cluster) + dir->sectorInClu;
   if (lba != dirSectorLba || vol->cacheEpoch != dirSectorEpoch) {
      if (readSectors(vol->storageHandle, lba, 1, dirSector) != 0) {
         dir->ioError = 1;   // a real I/O fault, NOT end-of-directory: the VFS boundary turns this into -1
         dir->cluster = 0;
         return 0;
      }
      dirSectorLba = lba;
      dirSectorEpoch  = vol->cacheEpoch;
   }
   const uint8_t *entry = &dirSector[dir->entryInSector * DIR_ENTRY_BYTES];

   uint32_t entriesPerSector = vol->bytesPerSector / DIR_ENTRY_BYTES;
   if (++dir->entryInSector >= entriesPerSector) {
      dir->entryInSector = 0;
      if (++dir->sectorInClu >= vol->sectorsPerCluster) {
         dir->sectorInClu = 0;
         dir->cluster = advanceDirCluster(dir);
      }
   }
   return entry;
}

// On-disk location of the entry set readExfatDir last returned (the 0x85 entry's
// position + the set size). Valid until the next readdir/backend call; shared like the
// sector caches, so only touched under the backend lock. The delete path reads this to
// find the entries to invalidate without re-parsing the directory.
static uint32_t lastSetCluster, lastSetSectorInClu, lastSetEntryInSector;
static int      lastSetCount, lastSetDirNoFatChain;

// Copies the position of the entry set readExfatDir most recently returned (the shared lastSet*
// globals) into `loc`. Valid only until the next readdir/backend call; caller holds the lock.
static void captureLastSet(ExfatEntryLoc *loc)
{
   loc->cluster       = lastSetCluster;
   loc->sectorInClu   = lastSetSectorInClu;
   loc->entryInSector = lastSetEntryInSector;
   loc->count         = lastSetCount;
   loc->dirNoFatChain = lastSetDirNoFatChain;
}

// Rolling exFAT SetChecksum: folds one 32-byte directory entry into `sum`. The primary (File)
// entry skips its own checksum field (bytes 2-3); secondary entries fold every byte.
static uint16_t addEntryChecksum(uint16_t sum, const uint8_t *entry, int isPrimary)
{
   for (int i = 0; i < DIR_ENTRY_BYTES; i++) {
      if (isPrimary && (i == 2 || i == 3)) continue;
      sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + entry[i]);
   }
   return sum;
}

int readExfatDir(ExfatDir *dir, char *name, int nameCap, ExfatInfo *info)
{
   for (;;) {
      uint32_t setCluster = dir->cluster, setSic = dir->sectorInClu, setEis = dir->entryInSector;
      const uint8_t *entry = getNextEntry(dir);
      if (!entry) return 0;

      uint8_t type = entry[0];
      if (type == ENTRY_END) {       // no further entries in this directory
         dir->cluster = 0;
         return 0;
      }
      if (type != ENTRY_FILE) continue;   // deleted entry or volume/bitmap/upcase

      // File directory entry: capture its fields before the next read overwrites
      // the shared sector buffer.
      uint8_t  secondaryCount = entry[1];
      if ((int)secondaryCount + 1 > MAX_SET_ENTRIES) continue;   // malformed: more entries than a set can hold
      uint16_t attributes     = readLe16(entry + 4);
      uint64_t mtime          = timestampToUnix(readLe32(entry + 12), entry[23]);   // LastModified + its UtcOffset
      int      isDir          = (attributes & ATTR_DIRECTORY) ? 1 : 0;

      // Fold the whole set into the SetChecksum as it is read (the primary skips its checksum
      // field) and reject the set if the on-disk SetChecksum doesn't match - a corrupt directory
      // entry is skipped rather than acted on.
      uint16_t storedChecksum = readLe16(entry + 2);
      uint16_t calcChecksum   = addEntryChecksum(0, entry, 1);

      const uint8_t *stream = getNextEntry(dir);
      if (!stream || stream[0] != ENTRY_STREAM) continue;   // malformed set
      calcChecksum = addEntryChecksum(calcChecksum, stream, 0);
      uint8_t  nameLength   = stream[3];
      // The set must carry enough File Name (0xC1) entries to hold the claimed NameLength: the spec
      // requires SecondaryCount == 1 (stream) + ceil(NameLength/15) name entries (vendor extensions
      // may add more, so this is a lower bound). Rejects a set that under-declares its name entries
      // (which would otherwise yield a truncated name that still happens to pass the SetChecksum).
      if (nameLength < 1 || (int)secondaryCount < 1 + ((int)nameLength + 14) / 15) continue;
      int      noFatChain   = (stream[1] & FLAG_NO_FAT_CHAIN) ? 1 : 0;
      uint32_t firstCluster = readLe32(stream + 20);
      uint64_t dataLength   = readLe64(stream + 24);
      uint64_t validLength  = readLe64(stream + 8);                  // ValidDataLength
      if (validLength > dataLength) validLength = dataLength;    // clamp a malformed value

      uint16_t utf16[256];
      int collected = 0;
      int truncated = 0;
      for (int remaining = (int)secondaryCount - 1; remaining > 0; remaining--) {
         const uint8_t *frag = getNextEntry(dir);
         if (!frag) { truncated = 1; break; }
         calcChecksum = addEntryChecksum(calcChecksum, frag, 0);
         if (frag[0] != ENTRY_NAME) continue;
         for (int i = 0; i < 15 && collected < (int)nameLength && collected < 255; i++)
            utf16[collected++] = readLe16(frag + 2 + i * 2);
      }
      utf16[collected] = 0;
      if (truncated || calcChecksum != storedChecksum) continue;   // incomplete or corrupt set: skip it

      utf16ToUtf8(utf16, name, nameCap);
      if (info) {
         info->size         = dataLength;
         info->mtime        = mtime;
         info->isDir        = isDir;
         info->firstCluster = firstCluster;
         info->noFatChain   = noFatChain;
         info->validSize    = validLength;
      }
      lastSetCluster       = setCluster;
      lastSetSectorInClu   = setSic;
      lastSetEntryInSector = setEis;
      lastSetCount         = (int)secondaryCount + 1;
      lastSetDirNoFatChain = dir->noFatChain;
      return 1;
   }
}

// Case-insensitive name compare through the volume's up-case table (full exFAT casefold, not just
// ASCII), so lookups match exactly the names the on-disk NameHash treats as equal. Defined below.
static int namesEqualFold(const ExfatVolume *vol, const char *a, const char *b);

// finds `target` (case-insensitive) directly in one directory; fills info.
static int findInDir(const ExfatVolume *vol, uint32_t dirCluster, int dirNoFatChain,
                     uint64_t dirByteLength, const char *target, ExfatInfo *info)
{
   ExfatDir dir;
   openExfatDir(&dir, vol, dirCluster, dirNoFatChain, dirByteLength);
   char name[256];
   while (readExfatDir(&dir, name, (int)sizeof(name), info)) {
      if (namesEqualFold(vol, name, target)) return 1;
   }
   return dir.ioError ? -1 : 0;   // distinguish a mid-scan I/O fault from a genuine miss
}

int statExfat(const ExfatVolume *vol, const char *path, ExfatInfo *info)
{
   const char *p = path;
   while (*p == '/') p++;

   if (*p == 0) {   // the root directory itself
      info->size         = 0;
      info->mtime        = 0;
      info->isDir        = 1;
      info->firstCluster = vol->rootCluster;
      info->noFatChain   = 0;
      info->validSize    = 0;
      return 0;
   }

   uint32_t dirCluster    = vol->rootCluster;
   int      dirNoFatChain = 0;
   uint64_t dirByteLength = 0;   // root: size unknown, bounded by the FAT cycle guard
   char     component[256];
   for (;;) {
      int n = 0;
      while (p[n] && p[n] != '/' && n < 255) { component[n] = p[n]; n++; }
      component[n] = 0;
      p += n;
      while (*p == '/') p++;

      if (n == 0) return -1;   // malformed (empty component)
      if (findInDir(vol, dirCluster, dirNoFatChain, dirByteLength, component, info) != 1) return -1;   // miss or I/O error
      if (*p == 0) return 0;            // last component matched
      if (!info->isDir) return -1;      // an intermediate must be a directory
      dirCluster    = info->firstCluster;
      dirNoFatChain = info->noFatChain;
      dirByteLength = info->size;       // bound the next level by this directory's DataLength
   }
}

// returns the cluster holding the file's cluster #index (0 if past the end).
static uint32_t getClusterAt(ExfatFile *file, uint32_t index)
{
   const ExfatVolume *vol = file->vol;
   uint32_t cluster, i;
   if (index >= file->cachedIndex && isClusterValid(vol, file->cachedCluster)) {
      cluster = file->cachedCluster;   // resume from the cached position
      i       = file->cachedIndex;
   } else {
      cluster = file->firstCluster;    // walk from the start
      i       = 0;
   }
   uint32_t walked = 0;
   while (i < index && isClusterValid(vol, cluster)) {
      cluster = file->noFatChain ? cluster + 1 : getNextCluster(vol, cluster);
      i++;
      if (++walked > vol->clusterCount) { cluster = 0; break; }   // cyclic/corrupt FAT chain: stop (mirrors the write-path guard)
   }
   file->cachedIndex   = index;
   file->cachedCluster = cluster;
   return cluster;
}

// Largest physically-contiguous span on disk, in sectors (capped at maxSectors), starting at
// file cluster #index, sector sectorInClu - so a multi-cluster read/write can issue ONE storage
// call instead of one per cluster. *lba gets the start LBA; the file's cluster cache is left on
// the last cluster of the span. Returns 0 if `index` is past the end. Uses the cluster+FAT caches.
static uint32_t findContigSpan(ExfatFile *file, uint32_t index, uint32_t sectorInClu,
                           uint32_t maxSectors, uint64_t *lba)
{
   ExfatVolume *vol = file->vol;
   uint32_t sectorsPerCluster = vol->sectorsPerCluster;
   uint32_t cluster = getClusterAt(file, index);
   if (!isClusterValid(vol, cluster)) return 0;
   *lba = clusterToSector(vol, cluster) + sectorInClu;
   uint32_t span = sectorsPerCluster - sectorInClu;            // sectors left in the start cluster
   uint32_t lastIndex = index;
   while (span < maxSectors) {                    // absorb following physically-adjacent clusters
      uint32_t nextCluster = file->noFatChain ? cluster + 1 : getNextCluster(vol, cluster);
      if (nextCluster != cluster + 1 || !isClusterValid(vol, nextCluster)) break;
      cluster = nextCluster; lastIndex++;
      span += sectorsPerCluster;
   }
   if (span > maxSectors) span = maxSectors;
   file->cachedIndex   = lastIndex;               // resume cache on the last cluster of the span
   file->cachedCluster = cluster;
   return span;
}

// True if `p` meets the lv2 storage DMA alignment, so it can be read/written in place
// without bouncing through an aligned scratch buffer.
static int isDmaAligned(const void *p)
{
   return ((uintptr_t)p & (STORAGE_ALIGN - 1)) == 0;
}

// Fills an open-file handle from a located/created entry: its data location (info) and
// the on-disk position of its entry set (entry, for flushing writes back).
static void setupFileHandle(ExfatFile *file, ExfatVolume *vol, const ExfatInfo *info, const ExfatEntryLoc *entry)
{
   file->vol           = vol;
   file->firstCluster  = info->firstCluster;
   file->size          = info->size;
   file->validSize     = info->validSize;
   file->position      = 0;
   file->noFatChain    = info->noFatChain;
   file->cachedIndex   = 0;
   file->cachedCluster = info->firstCluster;
   uint32_t clusterBytes         = getClusterBytes(vol);
   // size-derived count; reserveClusters resyncs it to the real chain length before extending
   file->allocClusters = (uint32_t)((info->size + clusterBytes - 1) / clusterBytes);
   file->entry         = *entry;
   file->writable      = 0;
   file->appendMode    = 0;
   file->dirty         = 0;
}

static int openOrCreateExfat(ExfatFile *file, ExfatVolume *vol, const char *path, int create);

int openExfat(ExfatFile *file, ExfatVolume *vol, const char *path)
{
   return openOrCreateExfat(file, vol, path, 0);
}

int readExfat(ExfatFile *file, void *buffer, int length)
{
   if (length < 0) return -1;
   const ExfatVolume *vol = file->vol;
   // vol is NULL once the device was yanked while this file was open (detachVolumeHandles), so
   // test it BEFORE dereferencing mounted - a late read on a stale handle must fail, not crash.
   if (!vol || !vol->mounted) return -1;

   uint64_t want = (uint64_t)length;
   uint64_t remaining = file->size - file->position;
   if (want > remaining) want = remaining;

   uint8_t *out = (uint8_t *)buffer;

   // exFAT: bytes in [validSize, size) read as zero. Serve disk bytes only up to validSize and
   // zero-fill anything past it (diverges only for foreign files written with validSize < size).
   uint64_t zeroFill = 0;
   if (file->position >= file->validSize) {        // wholly inside the zero tail
      memSet(out, 0, (int)want);
      file->position += want;
      return (int)want;
   }
   if (file->position + want > file->validSize) {  // read straddles validSize: split off the zero tail
      zeroFill = file->position + want - file->validSize;
      want    -= zeroFill;
   }

   uint32_t clusterBytes  = getClusterBytes(vol);
   uint32_t bytesPerSector = vol->bytesPerSector;
   uint32_t bounceSectors = EXFAT_READ_BOUNCE / bytesPerSector;   // cap when bouncing
   if (bounceSectors == 0) bounceSectors = 1;
   int total = 0;
   while (want > 0) {
      uint32_t index       = (uint32_t)(file->position / clusterBytes);
      uint32_t offsetInClu = (uint32_t)(file->position % clusterBytes);
      uint32_t sectorInClu = offsetInClu / bytesPerSector;
      uint32_t offsetInSec = offsetInClu % bytesPerSector;

      // Fast path: aligned destination on a sector boundary with at least a full sector to go.
      // Read as many physically-contiguous sectors as we can straight into the caller's buffer
      // (no bounce, spanning whole clusters) - one storage call for a multi-cluster run.
      if (offsetInSec == 0 && want >= bytesPerSector && isDmaAligned(out + total)) {
         uint32_t maxSectors = (uint32_t)(want / bytesPerSector);
         uint64_t lba;
         uint32_t run = findContigSpan(file, index, sectorInClu, maxSectors, &lba);
         if (run == 0) break;
         if (readSectors(vol->storageHandle, lba, run, out + total) != 0) break;
         uint32_t n = run * bytesPerSector;
         total += (int)n; file->position += n; want -= n;
         continue;
      }

      // Slow path: partial leading/trailing sector, or an unaligned caller buffer - bounce one
      // contiguous run (capped at the bounce) through the aligned scratch, then copy out.
      uint32_t cluster = getClusterAt(file, index);
      if (!isClusterValid(vol, cluster)) break;
      uint64_t lba = clusterToSector(vol, cluster) + sectorInClu;
      uint32_t runSectors = vol->sectorsPerCluster - sectorInClu;
      if (runSectors > bounceSectors) runSectors = bounceSectors;
      if (readSectors(vol->storageHandle, lba, runSectors, fileSector) != 0) break;
      uint32_t n = runSectors * bytesPerSector - offsetInSec;
      if ((uint64_t)n > want) n = (uint32_t)want;
      memCopy(out + total, fileSector + offsetInSec, (int)n);
      total += (int)n; file->position += n; want -= n;
   }

   // append the zero tail once the disk-backed portion is fully served (want hit 0, no I/O break)
   if (want == 0 && zeroFill > 0) {
      memSet(out + total, 0, (int)zeroFill);
      total += (int)zeroFill;
      file->position += zeroFill;
   }
   return total;
}

void seekExfat(ExfatFile *file, uint64_t position)
{
   if (position > file->size) position = file->size;
   file->position = position;
}

static int flushEntry(ExfatFile *file);   // defined in the write section below

int closeExfat(ExfatFile *file)
{
   int rc = 0;
   if (file->dirty && file->vol && file->vol->mounted) rc = flushEntry(file);
   file->dirty = 0;
   file->vol   = 0;
   return rc;
}

// Counts free clusters by popcounting the whole allocation bitmap, reading it through the 32 KB
// bounce (many sectors per storage call, ~2048 sectors on a 256 GB volume becomes a couple dozen
// reads). Uses vol->bitmapCluster/bitmapBytes recorded at mount (no root rescan). Returns 0 / -1.
static int countFreeClustersOnDisk(const ExfatVolume *vol, uint32_t *outFree)
{
   if (vol->bitmapCluster == 0) return -1;
   uint64_t freeClusters = 0;
   uint32_t bitsLeft     = vol->clusterCount;
   uint64_t bytesLeft    = ((uint64_t)vol->clusterCount + 7) / 8;
   if (bytesLeft > vol->bitmapBytes) bytesLeft = vol->bitmapBytes;
   uint32_t cluster = vol->bitmapCluster;
   uint32_t bounceSectors = EXFAT_READ_BOUNCE / vol->bytesPerSector;
   if (bounceSectors == 0) bounceSectors = 1;

   while (bytesLeft > 0 && isClusterValid(vol, cluster)) {
      uint64_t clusterLba = clusterToSector(vol, cluster);
      for (uint32_t s = 0; s < vol->sectorsPerCluster && bytesLeft > 0; ) {
         uint32_t runSectors = vol->sectorsPerCluster - s;
         if (runSectors > bounceSectors) runSectors = bounceSectors;
         if (readSectors(vol->storageHandle, clusterLba + s, runSectors, fileSector) != 0) return -1;
         uint64_t chunk = (uint64_t)runSectors * vol->bytesPerSector;
         if (chunk > bytesLeft) chunk = bytesLeft;
         for (uint64_t b = 0; b < chunk; b++) {
            uint8_t byte = fileSector[b];
            if (bitsLeft >= 8) {                 // whole byte of real clusters: count zero bits at once
               freeClusters += 8u - (uint32_t)__builtin_popcount(byte);
               bitsLeft -= 8;
            } else {                             // final partial byte: count only the valid bits
               for (int bit = 0; bit < 8 && bitsLeft > 0; bit++, bitsLeft--)
                  if (!((byte >> bit) & 1)) freeClusters++;
            }
         }
         bytesLeft -= chunk;
         s += runSectors;
      }
      cluster = getNextCluster(vol, cluster);   // bitmap may span clusters
   }
   *outFree = (uint32_t)freeClusters;
   return 0;
}

int getExfatFree(const ExfatVolume *vol, uint64_t *freeBytes, uint64_t *totalBytes)
{
   uint32_t clusterBytes = getClusterBytes(vol);
   if (totalBytes) *totalBytes = (uint64_t)vol->clusterCount * clusterBytes;
   if (vol->bitmapCluster == 0) return -1;   // no usable bitmap (loadVolumeMeta rejected it)
   // O(1): return the running free-cluster total seeded at mount and kept by markClusterRun, instead
   // of rescanning the whole bitmap on every query (the file manager polls free space on each refresh).
   if (freeBytes) *freeBytes = (uint64_t)vol->freeClusters * clusterBytes;
   return 0;
}

// ============================================================================
// Write support
//
// All of the below runs under the backend lock (the mutating VfsOps take it) and
// uses the shared writeScratch buffer for read-modify-write of bitmap / FAT /
// directory sectors. Offsets and the SetChecksum / NameHash algorithms come from
// the Microsoft exFAT specification; the lv2 write syscall (603) was taken from
// the in-tree references (see writeStorageRaw).
// ============================================================================

#define ENTRY_VOLLABEL  0x83          // volume label (primary)
#define ENTRY_UPCASE    0x82          // up-case table (primary)
#define ATTR_DIR_BITS   0x10          // FileAttributes: Directory
#define ATTR_ARCHIVE    0x20          // FileAttributes: Archive (set on created files)
#define STREAM_FLAGS_NEW (0x01 | FLAG_NO_FAT_CHAIN)   // AllocationPossible + NoFatChain (contiguous)
#define STREAM_FLAGS_CHAIN 0x01       // AllocationPossible, FAT-chained (data follows the FAT)
#define STREAM_FLAGS_EMPTY 0x00       // no allocation (0-length file): FirstCluster/DataLength must be 0
#define NAME_PER_ENTRY  15            // UTF-16 units per File Name entry

// A valid placeholder DOS timestamp (2025-01-01 00:00:00), used only as the fallback when the RTC
// is unavailable (getNowTimestamp reads the real time otherwise). fsck only requires a structurally
// valid timestamp.
#define EXFAT_DEFAULT_TS 0x5A210000u

static void loadVolumeMeta(ExfatVolume *vol)
{
   vol->bitmapCluster = 0; vol->bitmapBytes = 0;
   vol->upcaseCluster = 0; vol->upcaseBytes = 0;
   vol->label[0]      = 0;

   ExfatDir dir;
   openExfatDir(&dir, vol, vol->rootCluster, 0, 0);
   for (;;) {
      const uint8_t *entry = getNextEntry(&dir);
      if (!entry || entry[0] == ENTRY_END) break;
      if (entry[0] == ENTRY_BITMAP) {
         uint32_t bmCluster = readLe32(entry + 20);
         uint64_t bmBytes   = readLe64(entry + 24);
         // Validate before trusting: the bitmap drives every alloc/free, so a misplaced FirstCluster
         // or an undersized length would let bit flips scribble inside a victim file's data. Require a
         // real start cluster and a length that covers at least one bit per cluster. A bad bitmap
         // leaves bitmapCluster == 0, which disables writes (markClusterRun returns -1) but keeps
         // reads working - safer than acting on a hostile pointer.
         uint64_t bmBytesNeeded = ((uint64_t)vol->clusterCount + 7) / 8;
         if (isClusterValid(vol, bmCluster) && bmBytes >= bmBytesNeeded) {
            vol->bitmapCluster = bmCluster;
            vol->bitmapBytes   = bmBytes;
         }
      } else if (entry[0] == ENTRY_UPCASE) {
         vol->upcaseCluster = readLe32(entry + 20);
         vol->upcaseBytes   = readLe64(entry + 24);
      } else if (entry[0] == ENTRY_VOLLABEL) {
         // Volume Label entry: offset 1 = character count (UTF-16 units, <= 11),
         // offset 2 = label. Decode to UTF-8 for display.
         int count = entry[1];
         if (count > 11) count = 11;
         uint16_t units[12];
         for (int i = 0; i < count; i++) units[i] = readLe16(entry + 2 + i * 2);
         units[count] = 0;
         utf16ToUtf8(units, vol->label, (int)sizeof(vol->label));
      }
   }
}

// Maps a data cluster to its allocation-bitmap bit: the LBA of the bitmap sector, the byte
// within it, and the bit. The bitmap is a primary entry, so its clusters follow the FAT
// chain. Returns 0 / -1. (Pass a hint cluster/base in *bitmapCluster/*byteBase to resume the chain walk
// across a sequential run; both must be {bitmapCluster, 0} on the first call.)
static int locateClusterBit(const ExfatVolume *vol, uint32_t cluster, uint32_t *bitmapCluster, uint64_t *byteBase,
                             uint64_t *lba, uint32_t *off, int *bit)
{
   if (!isClusterValid(vol, cluster) || vol->bitmapCluster == 0) return -1;
   uint64_t bitIndex  = (uint64_t)(cluster - 2);
   uint64_t byteIndex = bitIndex / 8;
   *bit = (int)(bitIndex & 7);
   uint32_t clusterBytes = getClusterBytes(vol);
   while (byteIndex >= *byteBase + clusterBytes) {     // walk forward to the bitmap cluster holding byteIndex
      *byteBase += clusterBytes;
      *bitmapCluster = getNextCluster(vol, *bitmapCluster);
      if (!isClusterValid(vol, *bitmapCluster)) return -1;
   }
   uint64_t within = byteIndex - *byteBase;
   *lba = clusterToSector(vol, *bitmapCluster) + within / vol->bytesPerSector;
   *off = (uint32_t)(within % vol->bytesPerSector);
   return 0;
}

// Sets (set!=0) or clears the bits for the contiguous cluster run [first, first+count),
// reading each affected bitmap sector once and writing it once - so allocating/freeing a
// large extent costs ~1 bitmap write per 4096 clusters instead of one per cluster. Returns 0 / -1.
static int markClusterRun(ExfatVolume *vol, uint32_t first, uint32_t count, int set)
{
   if (vol->bitmapCluster == 0) return -1;
   uint32_t bitmapCluster = vol->bitmapCluster; uint64_t base = 0;
   uint64_t curLba = ~0ULL; int dirty = 0;
   for (uint32_t k = 0; k < count; k++) {
      uint64_t lba; uint32_t off; int bit;
      if (locateClusterBit(vol, first + k, &bitmapCluster, &base, &lba, &off, &bit) != 0) {
         if (dirty) writeSectors(vol->storageHandle, curLba, 1, writeScratch);
         return -1;
      }
      if (lba != curLba) {
         if (dirty && writeSectors(vol->storageHandle, curLba, 1, writeScratch) != 0) return -1;
         if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
         curLba = lba; dirty = 0;
      }
      if (set) writeScratch[off] |= (uint8_t)(1u << bit);
      else     writeScratch[off] &= (uint8_t)~(1u << bit);
      dirty = 1;
   }
   if (dirty && writeSectors(vol->storageHandle, curLba, 1, writeScratch) != 0) return -1;
   // keep the running free-cluster total in sync (callers only set free runs / clear used runs, so the
   // whole `count` changes state). Only on full success; a mid-run failure above leaves it for remount.
   if (set) vol->freeClusters -= count;
   else     vol->freeClusters += count;
   return 0;
}

// Ensures the bitmap read window in fileSector covers sector `lba` (which lies in bitmap cluster
// `bitmapCluster`), reading up to a 32 KB span that never crosses the (FAT-chained) bitmap cluster
// boundary - so a bitmap scan costs ~1 read per 64 sectors instead of one per sector. *bufLba/*bufCount
// track the buffered window (start them {0,0}); *winBase returns the byte offset of `lba`'s sector
// within fileSector. Returns 0 / -1 (I/O).
static int ensureBitmapWindow(const ExfatVolume *vol, uint32_t bitmapCluster, uint64_t lba,
                              uint64_t *bufLba, uint32_t *bufCount, uint32_t *winBase)
{
   if (*bufCount == 0 || lba < *bufLba || lba >= *bufLba + *bufCount) {
      uint32_t bounceSectors = EXFAT_READ_BOUNCE / vol->bytesPerSector;
      if (bounceSectors == 0) bounceSectors = 1;
      uint32_t within = (uint32_t)(lba - clusterToSector(vol, bitmapCluster));   // sector within the bitmap cluster
      uint32_t remain = vol->sectorsPerCluster - within;                         // never read past this cluster
      uint32_t count  = remain < bounceSectors ? remain : bounceSectors;
      if (readSectors(vol->storageHandle, lba, count, fileSector) != 0) return -1;
      *bufLba = lba; *bufCount = count;
   }
   *winBase = (uint32_t)(lba - *bufLba) * vol->bytesPerSector;
   return 0;
}

// Counts how many clusters starting at `first` are free in the bitmap, up to `max`
// (the length of the contiguous free run beginning there). 0 on error / none free.
static uint32_t countFreeClusters(ExfatVolume *vol, uint32_t first, uint32_t max)
{
   if (vol->bitmapCluster == 0) return 0;
   uint32_t bitmapCluster = vol->bitmapCluster; uint64_t base = 0;
   uint64_t bufLba = 0; uint32_t bufCount = 0;
   uint32_t run = 0;
   for (uint32_t k = 0; k < max; k++) {
      uint32_t cluster = first + k;
      if (!isClusterValid(vol, cluster)) break;
      uint64_t lba; uint32_t off; int bit; uint32_t winBase;
      if (locateClusterBit(vol, cluster, &bitmapCluster, &base, &lba, &off, &bit) != 0) break;
      if (ensureBitmapWindow(vol, bitmapCluster, lba, &bufLba, &bufCount, &winBase) != 0) break;
      if ((fileSector[winBase + off] >> bit) & 1) break;   // used: run ends
      run++;
   }
   return run;
}

// Finds the first free-cluster run at/after `from`, returning its first cluster and length
// (capped at `want`) in *got. Read-only scan. 0 if no free cluster in [from, clusterCount+2).
static uint32_t findFreeRun(ExfatVolume *vol, uint32_t from, uint32_t want, uint32_t *got)
{
   uint32_t bitmapCluster = vol->bitmapCluster; uint64_t base = 0;
   uint64_t bufLba = 0; uint32_t bufCount = 0;
   uint32_t runStart = 0, runLen = 0;
   if (from < 2) from = 2;
   for (uint32_t c = from; c < vol->clusterCount + 2; c++) {
      uint64_t lba; uint32_t off; int bit; uint32_t winBase;
      if (locateClusterBit(vol, c, &bitmapCluster, &base, &lba, &off, &bit) != 0) break;
      if (ensureBitmapWindow(vol, bitmapCluster, lba, &bufLba, &bufCount, &winBase) != 0) return 0;
      if (!((fileSector[winBase + off] >> bit) & 1)) {              // free
         if (runLen == 0) runStart = c;
         if (++runLen >= want) { *got = runLen; return runStart; }
      } else if (runLen > 0) {                            // run ended before reaching `want`
         *got = runLen; return runStart;
      }
   }
   if (runLen > 0) { *got = runLen; return runStart; }
   return 0;
}

// Allocates a contiguous run of up to `want` free clusters near the alloc hint (wrapping to
// the start of the volume), marking them used in bulk. Returns the run's first cluster (0 if
// the volume is full / on error); *got = clusters allocated (1..want). For large files this
// replaces ~want per-cluster bitmap+FAT writes with a couple of bitmap-sector writes.
static uint32_t allocClusterRun(ExfatVolume *vol, uint32_t want, uint32_t *got)
{
   *got = 0;
   if (vol->bitmapCluster == 0 || want == 0) return 0;
   uint32_t hint  = vol->allocHint >= 2 ? vol->allocHint : 2;
   uint32_t first = findFreeRun(vol, hint, want, got);
   if (first == 0 && hint > 2) first = findFreeRun(vol, 2, want, got);   // wrap to reuse earlier frees
   if (first == 0) return 0;
   if (markClusterRun(vol, first, *got, 1) != 0) { *got = 0; return 0; }
   vol->allocHint = first + *got;
   return first;
}

static int freeCluster(ExfatVolume *vol, uint32_t cluster)
{
   if (cluster >= 2 && cluster < vol->allocHint) vol->allocHint = cluster;   // reuse freed space first
   invalidateDirEnd();   // a freed cluster may be reused; never let the end cache point at it
   return markClusterRun(vol, cluster, 1, 0);
}

// Allocates a single free cluster near the alloc hint (a run of one). Returns the cluster
// (>= 2), or 0 if the volume is full / on error.
static uint32_t allocCluster(ExfatVolume *vol)
{
   uint32_t got;
   return allocClusterRun(vol, 1, &got);
}

// Zeroes every sector of `cluster` (a freshly-allocated, empty directory: its
// first entry being 0x00 marks the end of directory). Returns 0 / -1.
static int zeroCluster(ExfatVolume *vol, uint32_t cluster)
{
   if (!isClusterValid(vol, cluster)) return -1;
   // Zero the whole cluster in multi-sector writes through the 32 KB bounce instead of one
   // sector per call (64 syscalls -> 1-2 for a 32 KB cluster at 512-byte sectors).
   uint32_t chunkSectors = EXFAT_READ_BOUNCE / vol->bytesPerSector;
   if (chunkSectors == 0) chunkSectors = 1;
   memSet(fileSector, 0, (int)(chunkSectors * vol->bytesPerSector));
   uint64_t cs = clusterToSector(vol, cluster);
   for (uint32_t s = 0; s < vol->sectorsPerCluster; ) {
      uint32_t run = vol->sectorsPerCluster - s;
      if (run > chunkSectors) run = chunkSectors;
      if (writeSectors(vol->storageHandle, cs + s, run, fileSector) != 0) return -1;
      s += run;
   }
   invalidateCaches();
   return 0;
}

// Decompresses the volume's on-disk up-case table (run-length compressed with the 0xFFFF marker)
// into the shared upcaseTable[] cache, once per storageHandle. Entries outside the table's defined range stay
// identity. Returns 0 / -1 (I/O). After this, upcaseTable[c] is the up-cased form of any unit c.
// Decodes the on-disk $UpCase into the sparse cache: only code points whose up-cased form differs from
// identity are recorded (in ascending code-point order, ready for binary search). The on-disk table is
// the run-length form (a 0xFFFF marker means the next u16 is a count of identity code points to skip).
static int ensureUpcaseTable(const ExfatVolume *vol)
{
   if (upcaseTableEpoch == vol->cacheEpoch) return 0;                                // already cached
   upcaseCount = 0;                                  // identity default = empty sparse table
   if (vol->upcaseCluster == 0 || vol->upcaseBytes == 0) { upcaseTableEpoch = vol->cacheEpoch; return 0; }

   uint64_t total    = vol->upcaseBytes & ~1ULL;    // whole u16 units only
   uint64_t consumed = 0;
   uint32_t cp       = 0;                            // current code point index
   int      pending  = 0;                            // next u16 is an identity run length
   uint32_t upcaseCluster       = vol->upcaseCluster;
   uint32_t guard    = 0;

   while (isClusterValid(vol, upcaseCluster) && consumed < total && cp < 0x10000) {
      uint64_t cs = clusterToSector(vol, upcaseCluster);
      for (uint32_t s = 0; s < vol->sectorsPerCluster && consumed < total && cp < 0x10000; s++) {
         if (readSectors(vol->storageHandle, cs + s, 1, writeScratch) != 0) return -1;
         uint32_t span = vol->bytesPerSector;
         for (uint32_t o = 0; o + 1 < span && consumed < total && cp < 0x10000; o += 2, consumed += 2) {
            uint16_t v = readLe16(writeScratch + o);
            if (pending) { cp += v; pending = 0; continue; }   // identity run of `v` code points
            if (v == 0xFFFF) { pending = 1; continue; }
            if (v != (uint16_t)cp && upcaseCount < EXFAT_UPCASE_MAX) {   // store only real mappings
               upcaseCp[upcaseCount] = (uint16_t)cp;
               upcaseUp[upcaseCount] = v;
               upcaseCount++;
            }
            cp++;
         }
      }
      if (++guard > vol->clusterCount) break;   // corrupt/cyclic up-case chain: stop
      upcaseCluster = getNextCluster(vol, upcaseCluster);
   }
   upcaseTableEpoch = vol->cacheEpoch;
   return 0;
}

// Up-cased form of UTF-16 unit `c` via the sparse table (binary search; identity if not mapped).
static uint16_t upcaseOf(uint16_t c)
{
   int lo = 0, hi = (int)upcaseCount - 1;
   while (lo <= hi) {
      int mid = (lo + hi) >> 1;
      uint16_t key = upcaseCp[mid];
      if (key == c) return upcaseUp[mid];
      if (key < c) lo = mid + 1; else hi = mid - 1;
   }
   return c;
}

// Up-cases a UTF-16 name through the cached up-case table. `out` is filled with the up-cased
// units (identity where the table has no mapping or it couldn't be read). For the NameHash.
static void upcaseName(const ExfatVolume *vol, const uint16_t *in, int len, uint16_t *out)
{
   if (ensureUpcaseTable(vol) != 0) { for (int k = 0; k < len; k++) out[k] = in[k]; return; }
   for (int k = 0; k < len; k++) out[k] = upcaseOf(in[k]);
}

// exFAT NameHash over the up-cased name (little-endian bytes of each unit).
static uint16_t getNameHash(const uint16_t *upName, int len)
{
   uint16_t hash = 0;
   for (int k = 0; k < len; k++) {
      uint8_t lo = (uint8_t)(upName[k] & 0xFF);
      uint8_t hi = (uint8_t)(upName[k] >> 8);
      hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + lo);
      hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + hi);
   }
   return hash;
}

// exFAT SetChecksum over `count` 32-byte entries, skipping the two checksum bytes
// (offsets 2 and 3) in the primary entry.
static uint16_t computeChecksum(const uint8_t *entries, int count)
{
   uint16_t sum = 0;
   for (int i = 0; i < count; i++)
      sum = addEntryChecksum(sum, entries + i * DIR_ENTRY_BYTES, i == 0);
   return sum;
}

// Number of UTF-16 units in a NUL-terminated UTF-16 string (capped at 255).
static int getUtf16Len(const uint16_t *s)
{
   int n = 0;
   while (s[n] && n < 255) n++;
   return n;
}

// Case-insensitive name compare via the volume's up-case table (full exFAT casefold). Falls back
// to ASCII folding only if the table can't be read. Returns 1 if the names are equal, 0 if not.
static int namesEqualFold(const ExfatVolume *vol, const char *a, const char *b)
{
   uint16_t a16[256], b16[256];
   utf8ToUtf16(a, a16, 255);
   utf8ToUtf16(b, b16, 255);
   int la = getUtf16Len(a16), lb = getUtf16Len(b16);
   if (la != lb) return 0;
   if (ensureUpcaseTable(vol) != 0) return strCmpICase(a, b) == 0;   // I/O failure: ASCII fallback
   for (int i = 0; i < la; i++)
      if (upcaseOf(a16[i]) != upcaseOf(b16[i])) return 0;
   return 1;
}

// Fills lbas[]/offs[] with the on-disk location of loc->count consecutive 32-byte
// entries starting at loc, stepping across sectors and clusters (FAT chain or
// contiguous per loc->dirNoFatChain). Returns 0, or -1 if the directory runs out of
// clusters before count entries (no extension yet).
static int collectSetSlots(const ExfatVolume *vol, const ExfatEntryLoc *loc, uint64_t *lbas, uint32_t *offs)
{
   if (loc->count < 1 || loc->count > MAX_SET_ENTRIES) return -1;   // keep within the fixed lbas[]/offs[]
   uint32_t entriesPerSector = vol->bytesPerSector / DIR_ENTRY_BYTES;
   uint32_t cluster = loc->cluster, sectorInClu = loc->sectorInClu, entryInSector = loc->entryInSector;
   for (int i = 0; i < loc->count; i++) {
      if (!isClusterValid(vol, cluster)) return -1;
      lbas[i] = clusterToSector(vol, cluster) + sectorInClu;
      offs[i] = entryInSector * DIR_ENTRY_BYTES;
      if (++entryInSector >= entriesPerSector) {
         entryInSector = 0;
         if (++sectorInClu >= vol->sectorsPerCluster) {
            sectorInClu = 0;
            cluster = loc->dirNoFatChain ? (isClusterValid(vol, cluster + 1) ? cluster + 1 : 0)
                                         : getNextCluster(vol, cluster);
         }
      }
   }
   return 0;
}

// Reads a directory entry set (loc->count entries) into out[]. Entries that share a sector are
// read once (a typical 3-entry set lives in one sector, so one read instead of three).
static int readEntrySet(const ExfatVolume *vol, const ExfatEntryLoc *loc, uint8_t *out)
{
   uint64_t lbas[MAX_SET_ENTRIES];
   uint32_t offs[MAX_SET_ENTRIES];
   if (collectSetSlots(vol, loc, lbas, offs) != 0) return -1;
   int i = 0;
   while (i < loc->count) {                              // same-sector entries are contiguous in i
      uint64_t lba = lbas[i];
      if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
      do {
         memCopy(out + i * DIR_ENTRY_BYTES, writeScratch + offs[i], DIR_ENTRY_BYTES);
         i++;
      } while (i < loc->count && lbas[i] == lba);
   }
   return 0;
}

// Writes an entry set, the primary (0x85) sector LAST, so the set only becomes live once the
// primary lands. Entries sharing a sector are written in one read-modify-write; sectors are
// written highest-index-first so the sector holding the primary (index 0) goes out last. 0 / -1.
static int writeEntrySet(ExfatVolume *vol, const ExfatEntryLoc *loc, const uint8_t *set)
{
   uint64_t lbas[MAX_SET_ENTRIES];
   uint32_t offs[MAX_SET_ENTRIES];
   if (collectSetSlots(vol, loc, lbas, offs) != 0) return -1;
   int hi = loc->count - 1;
   while (hi >= 0) {
      uint64_t lba = lbas[hi];
      int lo = hi;
      while (lo > 0 && lbas[lo - 1] == lba) lo--;        // [lo, hi] all live in this sector
      if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
      for (int i = lo; i <= hi; i++)
         memCopy(writeScratch + offs[i], set + i * DIR_ENTRY_BYTES, DIR_ENTRY_BYTES);
      if (writeSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
      hi = lo - 1;
   }
   invalidateCaches();
   return 0;
}

// Rewrites a contiguous (NoFatChain) run of `count` clusters from firstCluster as an
// explicit FAT chain, so the allocation can be extended non-contiguously. Returns 0 / -1.
static int convertToChain(ExfatVolume *vol, uint32_t firstCluster, uint32_t count)
{
   // Validate every cluster of the run before writing its FAT entry. `count` may come from an
   // on-disk DataLength (a contiguous/Windows directory); a hostile or corrupt value would
   // otherwise drive setFat() at out-of-range clusters, whose FAT-entry LBA can land anywhere on
   // the device. Refuse the conversion rather than write outside the FAT region.
   for (uint32_t k = 0; k + 1 < count; k++) {
      if (!isClusterValid(vol, firstCluster + k) || !isClusterValid(vol, firstCluster + k + 1)) return -1;
      if (setFat(vol, firstCluster + k, firstCluster + k + 1) != 0) return -1;
   }
   if (!isClusterValid(vol, firstCluster + count - 1)) return -1;
   return setFat(vol, firstCluster + count - 1, EXFAT_EOC);
}

// Converts a contiguous (NoFatChain) directory to an explicit FAT chain and clears the
// NoFatChain flag in its entry, so it can be traversed and grown by following the FAT
// like any other directory. Returns 0 / -1. (Pre-existing/Windows dirs may be contiguous;
// our own mkdir already makes FAT-chain dirs.)
static int materializeDirChain(ExfatVolume *vol, uint32_t dirCluster, const ExfatEntryLoc *dirEntry)
{
   uint8_t  set[MAX_SET_ENTRIES * DIR_ENTRY_BYTES];
   if (readEntrySet(vol, dirEntry, set) != 0) return -1;
   uint8_t *stream = set + DIR_ENTRY_BYTES;
   uint32_t clusterBytes   = getClusterBytes(vol);
   // Derive the cluster count from the directory's DataLength, but cap it in 64-bit at clusterCount
   // BEFORE the 32-bit cast: a corrupt/hostile DataLength near UINT64_MAX would otherwise truncate
   // to an arbitrary count. No real directory allocation can exceed the volume's cluster count.
   uint64_t have64 = (readLe64(stream + 24) + clusterBytes - 1) / clusterBytes;
   if (have64 > vol->clusterCount) have64 = vol->clusterCount;
   uint32_t have = (uint32_t)have64;
   if (have == 0) have = 1;
   if (convertToChain(vol, dirCluster, have) != 0) return -1;
   stream[1] = (uint8_t)(stream[1] & ~FLAG_NO_FAT_CHAIN);
   writeLe16(set + 2, computeChecksum(set, dirEntry->count));
   return writeEntrySet(vol, dirEntry, set);
}

// Grows a (FAT-chained) directory by one zeroed cluster - its first entry, being 0x00,
// becomes the new end-of-directory marker - and bumps its DataLength in its own entry.
// dirEntry is NULL for the root, whose size is not tracked by any entry. Returns the new
// cluster, or 0 on out-of-space / I/O error.
static uint32_t extendDirectory(ExfatVolume *vol, uint32_t lastCluster, const ExfatEntryLoc *dirEntry)
{
   uint32_t newCluster = allocCluster(vol);
   if (newCluster == 0) return 0;
   if (zeroCluster(vol, newCluster) != 0) goto freeOnly;
   if (chainCluster(vol, lastCluster, newCluster) != 0) goto freeOnly;
   if (dirEntry) {
      uint8_t  set[MAX_SET_ENTRIES * DIR_ENTRY_BYTES];
      if (readEntrySet(vol, dirEntry, set) != 0) goto unchain;
      uint8_t *stream = set + DIR_ENTRY_BYTES;
      uint64_t newLen = readLe64(stream + 24) + getClusterBytes(vol);
      writeLe64(stream + 8,  newLen);   // ValidDataLength
      writeLe64(stream + 24, newLen);   // DataLength
      writeLe16(set + 2, computeChecksum(set, dirEntry->count));
      if (writeEntrySet(vol, dirEntry, set) != 0) goto unchain;
   }
   return newCluster;

   // failure cleanup: the size bump didn't land, so don't leave the cluster chained-but-unowned
unchain:
   setFat(vol, lastCluster, EXFAT_EOC);   // detach the cluster we appended before freeing it
freeOnly:
   freeCluster(vol, newCluster);
   return 0;
}

// Advances loc by `count` entries along the directory's FAT chain. Returns 0 if it stayed
// within the allocated chain, -1 if it ran off the end.
static int advanceLoc(const ExfatVolume *vol, ExfatEntryLoc *loc, int count)
{
   uint32_t entriesPerSector = vol->bytesPerSector / DIR_ENTRY_BYTES;
   for (int k = 0; k < count; k++) {
      if (++loc->entryInSector >= entriesPerSector) {
         loc->entryInSector = 0;
         if (++loc->sectorInClu >= vol->sectorsPerCluster) {
            loc->sectorInClu = 0;
            loc->cluster = getNextCluster(vol, loc->cluster);
            if (!isClusterValid(vol, loc->cluster)) return -1;
         }
      }
   }
   return 0;
}

// Places a built entry set at the directory's end-of-directory (0x00) marker, growing the
// directory by a cluster if the set doesn't fit. dirEntry locates the directory's own entry
// so its size can be updated on growth (NULL for the root). Caches the resulting end marker
// so a run of inserts into one directory is O(1) each instead of rescanning. Returns 0 / -1.
static int placeEntrySet(ExfatVolume *vol, uint32_t dirCluster, int dirNoFatChain,
                         const ExfatEntryLoc *dirEntry, const uint8_t *set, int setCount,
                         ExfatEntryLoc *placedOut)
{
   // make the directory a FAT chain so the scan/extend below follow the FAT uniformly
   // (the EOC marks the real end; contiguous traversal can't tell a dir cluster from a
   // neighbour's). The root is always FAT-chained; only a pre-existing contiguous dir converts.
   if (dirNoFatChain && dirEntry && materializeDirChain(vol, dirCluster, dirEntry) != 0) return -1;

   uint32_t entriesPerSector = vol->bytesPerSector / DIR_ENTRY_BYTES;
   ExfatEntryLoc loc = { dirCluster, 0, 0, setCount, 0 };
   int haveMarker = 0;

   // fast path: reuse this directory's cached end marker if it still reads as 0x00
   const ExfatEntryLoc *cachedEnd = findDirEnd(vol->cacheEpoch, dirCluster);
   if (cachedEnd && isClusterValid(vol, cachedEnd->cluster)) {
      uint64_t lba = clusterToSector(vol, cachedEnd->cluster) + cachedEnd->sectorInClu;
      if (readSectors(vol->storageHandle, lba, 1, writeScratch) == 0 &&
          writeScratch[cachedEnd->entryInSector * DIR_ENTRY_BYTES] == ENTRY_END) {
         loc.cluster       = cachedEnd->cluster;
         loc.sectorInClu   = cachedEnd->sectorInClu;
         loc.entryInSector = cachedEnd->entryInSector;
         haveMarker = 1;
      }
   }

   // otherwise scan from the start for the first end-of-directory marker
   if (!haveMarker) {
      for (;;) {
         if (!isClusterValid(vol, loc.cluster)) break;        // chain ended with no marker (dir full)
         uint64_t lba = clusterToSector(vol, loc.cluster) + loc.sectorInClu;
         if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
         if (writeScratch[loc.entryInSector * DIR_ENTRY_BYTES] == ENTRY_END) { haveMarker = 1; break; }
         if (++loc.entryInSector >= entriesPerSector) {
            loc.entryInSector = 0;
            if (++loc.sectorInClu >= vol->sectorsPerCluster) {
               loc.sectorInClu = 0;
               loc.cluster = getNextCluster(vol, loc.cluster);
            }
         }
      }
   }

   // the set fits only if a marker was found and its setCount slots stay within the chain
   uint64_t lbas[MAX_SET_ENTRIES];
   uint32_t offs[MAX_SET_ENTRIES];
   int fits = haveMarker && collectSetSlots(vol, &loc, lbas, offs) == 0;
   if (!fits) {
      uint32_t tail = haveMarker ? loc.cluster : dirCluster;   // grow after the true last cluster
      uint32_t guard = 0;
      for (uint32_t n; (n = getNextCluster(vol, tail)) != 0; ) {
         tail = n;
         if (++guard > vol->clusterCount) break;   // corrupt/cyclic directory chain: stop walking
      }
      uint32_t added = extendDirectory(vol, tail, dirEntry);
      if (added == 0) return -1;
      if (!haveMarker) { loc.cluster = added; loc.sectorInClu = 0; loc.entryInSector = 0; }
   }
   if (writeEntrySet(vol, &loc, set) != 0) { invalidateDirEnd(); return -1; }
   if (placedOut) *placedOut = loc;   // hand back where the set landed (the entry's on-disk location)

   // cache the new end-of-directory marker (one set past where we placed) for the next insert
   ExfatEntryLoc end = loc;
   if (advanceLoc(vol, &end, setCount) == 0) storeDirEnd(vol->cacheEpoch, dirCluster, &end);
   else                                       invalidateDirEnd();
   return 0;
}

// Splits an in-volume path into its parent directory path and leaf name. Returns 0,
// or -1 if the path is the root or its leaf is empty. parent is "/" for a top-level
// entry; otherwise the substring before the last '/'.
static int splitParentLeaf(const char *path, char *parent, int parentCap, char *leaf, int leafCap)
{
   const char *p = path;
   while (*p == '/') p++;
   if (*p == 0) return -1;                  // root has no parent/leaf

   int lastSlash = -1;
   for (int i = 0; path[i]; i++) if (path[i] == '/') lastSlash = i;

   const char *l = (lastSlash >= 0) ? path + lastSlash + 1 : path;
   int n = 0;
   while (l[n] && n < leafCap - 1) { leaf[n] = l[n]; n++; }
   leaf[n] = 0;
   if (n == 0) return -1;
   if (l[n] != 0) return -1;   // leaf didn't fit: refuse rather than operate on a truncated name

   if (lastSlash <= 0) { parent[0] = '/'; parent[1] = 0; }
   else {
      if (lastSlash > parentCap - 1) return -1;   // parent path doesn't fit
      int j = 0;
      for (; j < lastSlash && j < parentCap - 1; j++) parent[j] = path[j];
      parent[j] = 0;
   }
   return 0;
}

// Current time as an exFAT (DOS-form) timestamp, in UTC - the inverse of timestampToUnix. Entries
// are written with this plus a UtcOffset byte of 0x80 (OffsetValid, offset 0 = UTC), so the stored
// time is internally consistent and anchored for other readers. Falls back to EXFAT_DEFAULT_TS when
// the RTC is unavailable. Requires the host app to have loaded CELL_SYSMODULE_RTC (initRtc).
#define EXFAT_TZ_UTC 0x80   // UtcOffset byte: OffsetValid set, offset 0 (UTC)
static uint32_t getNowTimestamp(void)
{
   CellRtcDateTime now;
   if (cellRtcGetCurrentClock(&now, 0) != 0 || now.year < 1980) return EXFAT_DEFAULT_TS;
   return ((uint32_t)(now.year - 1980) << 25) | ((uint32_t)now.month  << 21) | ((uint32_t)now.day << 16)
        | ((uint32_t)now.hour  << 11) | ((uint32_t)now.minute << 5)  | ((uint32_t)now.second / 2);
}

// Given a set whose File (0x85) and Stream (0xC0) entries are already populated except
// for the name-derived fields, fills NameLength, NameHash, the Name (0xC1) entries,
// SecondaryCount and SetChecksum. Returns the entry count, or -1 on a bad name.
static int finalizeNamedSet(const ExfatVolume *vol, uint8_t *set, const char *leaf)
{
   uint16_t name16[256];
   utf8ToUtf16(leaf, name16, 255);
   int len = getUtf16Len(name16);
   if (len < 1 || len > 255) return -1;

   // Reject the exFAT-illegal File Name characters (spec Table 9: the control range 0x00-0x1F and
   // " * / : < > ? \ |), so a hostile/malformed name can't create an entry that confuses path
   // parsing or other readers. "." and ".." as whole names are also refused (no on-disk dot entries).
   for (int k = 0; k < len; k++) {
      uint16_t c = name16[k];
      if (c < 0x20) return -1;
      if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
          c == '>' || c == '?' || c == '\\' || c == '|') return -1;
   }
   if (name16[0] == '.' && (len == 1 || (len == 2 && name16[1] == '.'))) return -1;

   uint16_t up16[256];
   upcaseName(vol, name16, len, up16);

   int nameEntries = (len + NAME_PER_ENTRY - 1) / NAME_PER_ENTRY;
   int setCount    = 2 + nameEntries;

   set[1] = (uint8_t)(setCount - 1);               // File: SecondaryCount
   uint8_t *stream = set + DIR_ENTRY_BYTES;
   stream[3] = (uint8_t)len;                        // Stream: NameLength (units)
   writeLe16(stream + 4, getNameHash(up16, len));           // Stream: NameHash

   for (int i = 0; i < nameEntries; i++) {
      uint8_t *ne = set + (2 + i) * DIR_ENTRY_BYTES;
      ne[0] = ENTRY_NAME;
      for (int j = 0; j < NAME_PER_ENTRY; j++) {
         int idx = i * NAME_PER_ENTRY + j;
         if (idx < len) writeLe16(ne + 2 + j * 2, name16[idx]);
      }
   }
   writeLe16(set + 2, computeChecksum(set, setCount));       // SetChecksum (over the whole set)
   return setCount;
}

// Builds a fresh File + Stream + Name entry set into `set` (which must hold
// MAX_SET_ENTRIES * DIR_ENTRY_BYTES). attr = FileAttributes; streamFlags =
// GeneralSecondaryFlags; firstCluster/dataLength describe the data (0/0 for an empty
// file). Stamps create/modify/access with the current time. Returns the entry count,
// or -1 on a bad name.
static int buildEntrySet(const ExfatVolume *vol, uint8_t *set, uint16_t attr, uint8_t streamFlags,
                         uint32_t firstCluster, uint64_t dataLength, const char *leaf)
{
   memSet(set, 0, MAX_SET_ENTRIES * DIR_ENTRY_BYTES);
   uint32_t timestamp = getNowTimestamp();

   set[0] = ENTRY_FILE;
   writeLe16(set + 4, attr);             // FileAttributes
   writeLe32(set + 8,  timestamp);              // CreateTimestamp
   writeLe32(set + 12, timestamp);              // LastModifiedTimestamp
   writeLe32(set + 16, timestamp);              // LastAccessedTimestamp
   set[22] = EXFAT_TZ_UTC;          // CreateUtcOffset (UTC, so the stored local time == UTC)
   set[23] = EXFAT_TZ_UTC;          // LastModifiedUtcOffset
   set[24] = EXFAT_TZ_UTC;          // LastAccessedUtcOffset

   uint8_t *stream = set + DIR_ENTRY_BYTES;
   stream[0] = ENTRY_STREAM;
   stream[1] = streamFlags;         // GeneralSecondaryFlags
   writeLe64(stream + 8, dataLength);    // ValidDataLength
   writeLe32(stream + 20, firstCluster); // FirstCluster
   writeLe64(stream + 24, dataLength);   // DataLength

   return finalizeNamedSet(vol, set, leaf);
}

// Resolves a destination directory path for an insert: fills *info with its metadata
// and *entry with the directory's own entry-set location so placeEntrySet can grow it.
// The root has no parent entry, so *hasEntry is 0 for it. Must capture the entry before
// any further readdir (which would overwrite the shared location). Returns 0 / -1.
static int resolveParentDir(ExfatVolume *vol, const char *parent, ExfatInfo *info,
                            ExfatEntryLoc *entry, int *hasEntry)
{
   if (statExfat(vol, parent, info) != 0 || !info->isDir) return -1;
   *hasEntry = !(parent[0] == '/' && parent[1] == '\0');
   if (*hasEntry) captureLastSet(entry);
   return 0;
}

int mkdirExfatPath(ExfatVolume *vol, const char *path)
{
   char parent[512], leaf[256];
   if (splitParentLeaf(path, parent, (int)sizeof parent, leaf, (int)sizeof leaf) != 0) return -1;

   // Resolve the parent directory (and its own entry, for growth) and reject a name clash.
   ExfatInfo parentInfo;
   ExfatEntryLoc parentEntry;
   int hasParentEntry;
   if (resolveParentDir(vol, parent, &parentInfo, &parentEntry, &hasParentEntry) != 0) return -1;
   ExfatInfo existing;
   int clash = findInDir(vol, parentInfo.firstCluster, parentInfo.noFatChain, parentInfo.size, leaf, &existing);
   if (clash < 0) return -1;                          // I/O error mid-scan: can't verify the clash, fail safe
   if (clash == 1) return existing.isDir ? -2 : -1;

   // declarations before the first goto so no jump crosses an initializer
   uint8_t set[MAX_SET_ENTRIES * DIR_ENTRY_BYTES];
   int setCount;

   // Allocate, zero and FAT-terminate the new directory's first cluster (a FAT-chained
   // directory so it can be grown later by following the FAT).
   ensureVolumeDirty(vol);
   uint32_t dirCluster = allocCluster(vol);
   if (dirCluster == 0) return -1;
   if (zeroCluster(vol, dirCluster) != 0 || setFat(vol, dirCluster, EXFAT_EOC) != 0) goto fail;

   // DataLength == ValidDataLength == one cluster.
   setCount = buildEntrySet(vol, set, ATTR_DIR_BITS, STREAM_FLAGS_CHAIN, dirCluster, getClusterBytes(vol), leaf);
   if (setCount < 0) goto fail;

   if (placeEntrySet(vol, parentInfo.firstCluster, parentInfo.noFatChain,
                     hasParentEntry ? &parentEntry : 0, set, setCount, 0) != 0) goto fail;
   return 0;

fail:
   freeCluster(vol, dirCluster);   // give back the directory's first cluster on any failure after alloc
   return -1;
}

// Builds and places an empty-file entry set (named `leaf`) into an already-resolved parent.
// On success records the entry set's on-disk location in *placedOut (NULL if not needed),
// so an open can set up its handle without re-scanning the directory. Returns 0 / -1.
static int placeEmptyFile(ExfatVolume *vol, const ExfatInfo *parent, const ExfatEntryLoc *parentEntry,
                          const char *leaf, ExfatEntryLoc *placedOut)
{
   uint8_t set[MAX_SET_ENTRIES * DIR_ENTRY_BYTES];
   int setCount = buildEntrySet(vol, set, ATTR_ARCHIVE, STREAM_FLAGS_EMPTY, 0, 0, leaf);
   if (setCount < 0) return -1;
   return placeEntrySet(vol, parent->firstCluster, parent->noFatChain, parentEntry, set, setCount, placedOut);
}

// Creates an empty (0-length) file named by an in-volume path. The parent must
// already exist. Returns 0 on success, -2 if a file of that name already exists,
// -1 on any other error (root path, name clashes with a directory, parent full,
// I/O). An empty file allocates no clusters: FirstCluster and DataLength stay 0.
// Caller holds exfatLock.
int createExfatPath(ExfatVolume *vol, const char *path)
{
   char parent[512], leaf[256];
   if (splitParentLeaf(path, parent, (int)sizeof parent, leaf, (int)sizeof leaf) != 0) return -1;

   ExfatInfo parentInfo;
   ExfatEntryLoc parentEntry;
   int hasParentEntry;
   if (resolveParentDir(vol, parent, &parentInfo, &parentEntry, &hasParentEntry) != 0) return -1;
   ExfatInfo existing;
   int clash = findInDir(vol, parentInfo.firstCluster, parentInfo.noFatChain, parentInfo.size, leaf, &existing);
   if (clash < 0) return -1;                          // I/O error mid-scan: can't verify the clash, fail safe
   if (clash == 1) return existing.isDir ? -1 : -2;   // a directory of that name blocks it; a file = "exists"

   ensureVolumeDirty(vol);
   return placeEmptyFile(vol, &parentInfo, hasParentEntry ? &parentEntry : 0, leaf, 0) == 0 ? 0 : -1;
}

// On-disk location and metadata of a found entry set (filled by locateEntrySet).
typedef struct {
   ExfatEntryLoc entry;   // position of the 0x85 entry + the set size
   ExfatInfo     info;
} ExsetFatLoc;

// Finds `target` (case-insensitive) in one directory and records the on-disk location
// of its entry set (via the readdir position capture). Returns 1 if found, 0 otherwise.
static int locateEntrySet(const ExfatVolume *vol, uint32_t dirCluster, int dirNoFatChain,
                          uint64_t dirByteLength, const char *target, ExsetFatLoc *loc)
{
   ExfatDir dir;
   openExfatDir(&dir, vol, dirCluster, dirNoFatChain, dirByteLength);
   char name[256];
   ExfatInfo info;
   while (readExfatDir(&dir, name, (int)sizeof name, &info) == 1) {
      if (namesEqualFold(vol, name, target)) {
         captureLastSet(&loc->entry);
         loc->info = info;
         return 1;
      }
   }
   return dir.ioError ? -1 : 0;   // -1 = mid-scan I/O fault (not "absent"); 0 = genuine miss
}

// Opens the file at an in-volume path into `file`, creating it empty if absent and `create`
// is set. A single directory lookup serves both the existence test and the open: when the
// name isn't there, the create reuses the placement location instead of re-scanning. Returns
// 0 on success; -1 if not found (and not creating), the path is the root or a directory, or
// on I/O / no-space. Caller holds exfatLock.
static int openOrCreateExfat(ExfatFile *file, ExfatVolume *vol, const char *path, int create)
{
   char parent[512], leaf[256];
   if (splitParentLeaf(path, parent, (int)sizeof parent, leaf, (int)sizeof leaf) != 0) return -1;

   ExfatInfo parentInfo;
   ExfatEntryLoc parentEntry;
   int hasParentEntry;
   if (resolveParentDir(vol, parent, &parentInfo, &parentEntry, &hasParentEntry) != 0) return -1;

   // one scan of the parent: open it if present, otherwise fall through to create
   ExsetFatLoc found;
   int located = locateEntrySet(vol, parentInfo.firstCluster, parentInfo.noFatChain, parentInfo.size, leaf, &found);
   if (located < 0) return -1;                          // I/O error mid-scan: don't fall through and create a dup
   if (located == 1) {
      if (found.info.isDir) return -1;                 // can't open a directory as a file
      setupFileHandle(file, vol, &found.info, &found.entry);
      return 0;
   }
   if (!create) return -1;

   ExfatEntryLoc placed;
   if (placeEmptyFile(vol, &parentInfo, hasParentEntry ? &parentEntry : 0, leaf, &placed) != 0) return -1;
   ExfatInfo created = { 0, 0, 0, 0, 0, 0 };   // size, mtime, isDir, firstCluster, noFatChain, validSize
   setupFileHandle(file, vol, &created, &placed);
   return 0;
}

// Marks an entry set deleted by clearing the InUse bit (0x80) of each entry, primary
// first so a crash mid-delete leaves the set already invalid. Returns 0 / -1.
static int clearEntrySet(ExfatVolume *vol, const ExfatEntryLoc *loc)
{
   uint64_t lbas[MAX_SET_ENTRIES];
   uint32_t offs[MAX_SET_ENTRIES];
   if (collectSetSlots(vol, loc, lbas, offs) != 0) return -1;
   int i = 0;
   while (i < loc->count) {                              // the primary's sector is first, cleared first
      uint64_t lba = lbas[i];
      if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
      do {
         writeScratch[offs[i]] &= 0x7F;   // 0x85->0x05, 0xC0->0x40, 0xC1->0x41
         i++;
      } while (i < loc->count && lbas[i] == lba);
      if (writeSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
   }
   invalidateCaches();
   return 0;
}

// Releases a data allocation: clears its bitmap bits, and for a FAT chain clears each
// FAT link too. A 0-length file (firstCluster 0) owns nothing. noFatChain clusters are
// contiguous; their count comes from the byte length (at least one for a directory).
static void freeClusterChain(ExfatVolume *vol, uint32_t firstCluster, int noFatChain, uint64_t byteLength)
{
   if (!isClusterValid(vol, firstCluster)) return;
   if (noFatChain) {
      uint32_t clusterBytes = getClusterBytes(vol);
      // Cap in 64-bit at clusterCount BEFORE the 32-bit cast: a corrupt/hostile DataLength near
      // UINT64_MAX would otherwise truncate to an arbitrary 32-bit count and drive the clamp loop
      // below for billions of iterations (a stall). No real allocation can exceed clusterCount.
      uint64_t clusters64 = (byteLength + clusterBytes - 1) / clusterBytes;
      if (clusters64 > vol->clusterCount) clusters64 = vol->clusterCount;
      uint32_t clusters = (uint32_t)clusters64;
      if (clusters == 0) clusters = 1;
      while (clusters > 0 && !isClusterValid(vol, firstCluster + clusters - 1)) clusters--;   // clamp to volume
      if (clusters == 0) return;
      markClusterRun(vol, firstCluster, clusters, 0);   // bulk free (one write per bitmap sector)
      if (firstCluster < vol->allocHint) vol->allocHint = firstCluster;   // reuse the freed space first
      invalidateDirEnd();                              // freed clusters may be reused
   } else {
      // FAT chain: walk it (cached FAT reads) and free the bitmap in contiguous runs - one write
      // per bitmap sector instead of one per cluster, which on a multi-GB file is the difference
      // between a few writes and hundreds of thousands. The freed clusters' FAT entries are left
      // as-is: exFAT's allocation authority is the bitmap, a cluster's FAT entry matters only
      // while it is allocated to a chain, and it is overwritten if the cluster is later re-chained.
      uint32_t cluster = firstCluster;
      uint32_t runStart = 0, runLen = 0;
      uint32_t guard = 0;
      while (isClusterValid(vol, cluster)) {
         if (++guard > vol->clusterCount) break;  // corrupt/cyclic chain: stop after at most every cluster
         uint32_t next = getNextCluster(vol, cluster);
         if (runLen == 0) { runStart = cluster; runLen = 1; }
         else if (cluster == runStart + runLen) runLen++;  // physically adjacent: extend run
         else { markClusterRun(vol, runStart, runLen, 0); runStart = cluster; runLen = 1; }
         cluster = next;
      }
      if (runLen > 0) markClusterRun(vol, runStart, runLen, 0);
      if (firstCluster < vol->allocHint) vol->allocHint = firstCluster;
      invalidateDirEnd();
   }
}

// True if a directory holds no live entries (exFAT stores no "."/".." so an empty
// directory yields nothing). Caller holds exfatLock.
static int isDirEmpty(const ExfatVolume *vol, uint32_t dirCluster, int dirNoFatChain, uint64_t dirByteLength)
{
   ExfatDir dir;
   openExfatDir(&dir, vol, dirCluster, dirNoFatChain, dirByteLength);
   char name[256];
   ExfatInfo info;
   int got = readExfatDir(&dir, name, (int)sizeof name, &info);
   return got == 0 && !dir.ioError;   // "empty" only on a clean end-of-dir, never on an I/O fault
}

// Removes the file (requireDir 0) or empty directory (requireDir 1) at an in-volume
// path: invalidates its entry set, then frees its clusters. Returns 0 on success, -2 if
// the entry is already absent (the op maps this to idempotent success), -1 on a bad path,
// type mismatch, non-empty directory, or I/O error. Caller holds the lock.
static int removeNamedEntry(ExfatVolume *vol, const char *path, int requireDir)
{
   char parent[512], leaf[256];
   if (splitParentLeaf(path, parent, (int)sizeof parent, leaf, (int)sizeof leaf) != 0) return -1;

   ExfatInfo parentInfo;
   if (statExfat(vol, parent, &parentInfo) != 0 || !parentInfo.isDir) return -1;

   ExsetFatLoc loc;
   int located = locateEntrySet(vol, parentInfo.firstCluster, parentInfo.noFatChain, parentInfo.size, leaf, &loc);
   if (located < 0) return -1;    // mid-scan I/O fault: a real failure, not "already gone"
   if (located == 0) return -2;   // already absent: idempotent delete (mapped to 0 by the op)
   if (loc.info.isDir != requireDir) return -1;
   if (requireDir && !isDirEmpty(vol, loc.info.firstCluster, loc.info.noFatChain, loc.info.size)) return -1;

   // Clear the entry first (the name disappears atomically-ish), then free its data.
   ensureVolumeDirty(vol);
   if (clearEntrySet(vol, &loc.entry) != 0) return -1;
   freeClusterChain(vol, loc.info.firstCluster, loc.info.noFatChain, loc.info.size);
   return 0;
}

int unlinkExfatPath(ExfatVolume *vol, const char *path) { return removeNamedEntry(vol, path, 0); }
int rmdirExfatPath (ExfatVolume *vol, const char *path) { return removeNamedEntry(vol, path, 1); }

// Rebuilds an entry set under a new name, preserving the File (0x85) and Stream (0xC0)
// entries verbatim (attributes, timestamps, allocation) and replacing only the name.
// Returns the new entry count, or -1 on a bad name.
static int rebuildSetWithName(const ExfatVolume *vol, const uint8_t *oldSet, uint8_t *newSet, const char *leaf)
{
   memSet(newSet, 0, MAX_SET_ENTRIES * DIR_ENTRY_BYTES);
   memCopy(newSet, oldSet, 2 * DIR_ENTRY_BYTES);   // File + Stream kept; name re-derived below
   return finalizeNamedSet(vol, newSet, leaf);
}

// True if `path` is a STRICT descendant of `ancestor` (compared component-wise, ASCII case-
// insensitive). Equal paths return 0 so a case-only rename is still allowed. Used to refuse
// moving a directory into its own subtree, which would orphan the subtree (no parent links exist
// to walk up, so this is a path-prefix guard).
static int isPathWithin(const char *ancestor, const char *path)
{
   for (;;) {
      while (*ancestor == '/') ancestor++;
      while (*path == '/')     path++;
      if (*ancestor == 0) return *path != 0;   // ancestor consumed: descendant iff path has more
      if (*path == 0)     return 0;            // path shorter than ancestor

      // compare one path component, folding ASCII case
      const char *a = ancestor, *p = path;
      while (*a && *a != '/' && *p && *p != '/') {
         char ca = *a, cp = *p;
         if (ca >= 'a' && ca <= 'z') ca -= 32;
         if (cp >= 'a' && cp <= 'z') cp -= 32;
         if (ca != cp) return 0;
         a++; p++;
      }
      if (!(*a == 0 || *a == '/') || !(*p == 0 || *p == '/')) return 0;   // components differ in length
      ancestor = a; path = p;
   }
}

// Repoints any open file handle bound to the entry set at `from` to its new location `to`
// (defined in the VFS-backend section with the file pool). Caller holds exfatLock.
static void repointOpenHandles(const ExfatVolume *vol, const ExfatEntryLoc *from, const ExfatEntryLoc *to);

// Renames/moves an entry within one volume: relocates its entry set to the destination
// name (and directory) while leaving its data clusters in place. Returns 0 on success,
// -1 on a bad path, a missing source, an existing destination, or I/O error. Caller
// holds exfatLock. Cross-volume moves are not a rename (the VFS copies + deletes those).
int renameExfatPath(ExfatVolume *vol, const char *fromPath, const char *toPath)
{
   char fromParent[512], fromLeaf[256], toParent[512], toLeaf[256];
   if (splitParentLeaf(fromPath, fromParent, (int)sizeof fromParent, fromLeaf, (int)sizeof fromLeaf) != 0) return -1;
   if (splitParentLeaf(toPath,   toParent,   (int)sizeof toParent,   toLeaf,   (int)sizeof toLeaf)   != 0) return -1;
   if (isPathWithin(fromPath, toPath)) return -1;   // can't move an entry into its own subtree (would orphan it)

   // resolve the source parent, and the destination parent (with its entry, for growth)
   ExfatInfo fromDir, toDir;
   ExfatEntryLoc toEntry;
   int hasToEntry;
   if (statExfat(vol, fromParent, &fromDir) != 0 || !fromDir.isDir) return -1;
   if (resolveParentDir(vol, toParent, &toDir, &toEntry, &hasToEntry) != 0) return -1;
   ExsetFatLoc src;
   if (locateEntrySet(vol, fromDir.firstCluster, fromDir.noFatChain, fromDir.size, fromLeaf, &src) != 1) return -1;   // missing source or I/O error

   // Refuse to clobber an existing destination (the caller deletes first if it wants that).
   // A case-only rename (file.txt -> File.txt) resolves to the source's own entry, since
   // names compare case-insensitively; allow that - it just rewrites the stored name.
   ExsetFatLoc dst;
   int dstFound = locateEntrySet(vol, toDir.firstCluster, toDir.noFatChain, toDir.size, toLeaf, &dst);
   if (dstFound < 0) return -1;   // couldn't determine if the destination exists: fail, don't risk a clobber
   if (dstFound == 1 &&
       !(dst.entry.cluster == src.entry.cluster && dst.entry.sectorInClu == src.entry.sectorInClu &&
         dst.entry.entryInSector == src.entry.entryInSector))
      return -1;

   // build the renamed set from the original, place it, then invalidate the old one.
   // the data clusters are untouched - they belong to the relocated entry now. The two set
   // buffers are static (reused under the backend lock) to keep this frame off the ~16 KB
   // plugin thread stack - it already carries four path buffers.
   ensureVolumeDirty(vol);
   static uint8_t oldSet[MAX_SET_ENTRIES * DIR_ENTRY_BYTES];
   if (readEntrySet(vol, &src.entry, oldSet) != 0) return -1;
   static uint8_t newSet[MAX_SET_ENTRIES * DIR_ENTRY_BYTES];
   int newCount = rebuildSetWithName(vol, oldSet, newSet, toLeaf);
   if (newCount < 0) return -1;

   ExfatEntryLoc placed;
   if (placeEntrySet(vol, toDir.firstCluster, toDir.noFatChain,
                     hasToEntry ? &toEntry : 0, newSet, newCount, &placed) != 0) return -1;
   if (clearEntrySet(vol, &src.entry) != 0) {
      clearEntrySet(vol, &placed);   // roll back the just-placed destination so the data isn't cross-linked
      return -1;
   }
   // An open handle to the moved file still holds the old (now-cleared) entry location; repoint it
   // to the new set so a later flush writes the live entry, not the freed source slot (cross-link).
   repointOpenHandles(vol, &src.entry, &placed);
   return 0;
}

// --- file write -------------------------------------------------------------

// Ensures the file has at least `need` clusters allocated, preferring ONE contiguous NoFatChain
// extent: a new file grabs a free run in bulk (no per-cluster FAT writes, the bitmap marked a
// sector at a time), and a still-contiguous file is extended in place while the following
// clusters are free. Only when the free space is fragmented does it convert the run to a FAT
// chain and append the rest link by link. To keep a streaming copy from issuing one bitmap
// update per write call, it OVER-reserves: it allocates a geometrically growing batch (capped at
// +32 MB) past `need`, so allocation touches the bitmap once per tens of MB; the unused tail is
// released on close (releaseClustersFrom in flushEntry). Write-through is preserved - every
// bitmap/FAT state is durable immediately, so crash-safety is unchanged. Records the new total
// in file->allocClusters and returns it (== need normally; < need only if the volume filled up).
// Caller holds the lock.
//
// Cap on how far a streaming write reserves past what it currently needs. At 32 KB clusters this
// is 32 MB of look-ahead: enough to make bitmap updates rare during a big copy, small enough that
// the tail released on close (and the transient over-allocation) is negligible.
#define EXFAT_WRITE_RESERVE_AHEAD 1024
static uint32_t reserveClusters(ExfatFile *file, uint32_t need)
{
   ExfatVolume *vol  = file->vol;
   uint32_t have     = file->allocClusters;
   if (have >= need) return have;
   uint32_t ahead    = have;                                   // grow ~2x...
   if (ahead > EXFAT_WRITE_RESERVE_AHEAD) ahead = EXFAT_WRITE_RESERVE_AHEAD;   // ...capped
   uint32_t target   = need + ahead;                           // best-effort; never forces a chain

   // Phase 1 - contiguous NoFatChain growth (toward target; success needs only `need`).
   if (file->firstCluster == 0 || file->noFatChain) {
      if (file->firstCluster == 0) {
         uint32_t got = 0;
         uint32_t start = allocClusterRun(vol, target, &got);
         if (start == 0) { file->allocClusters = 0; return 0; }
         file->firstCluster  = start;
         file->noFatChain    = 1;
         file->cachedIndex   = 0;
         file->cachedCluster = start;
         have = got;
      }
      while (have < target) {                                  // extend the run in place if free
         uint32_t start = file->firstCluster + have;
         uint32_t got   = countFreeClusters(vol, start, target - have);
         if (got == 0) break;
         if (markClusterRun(vol, start, got, 1) != 0) break;
         vol->allocHint = start + got;
         have += got;
      }
      if (have >= need) { file->allocClusters = have; return have; }   // enough (maybe < target: fine)
      // fragmented: turn the contiguous run into a FAT chain, then append the rest by chaining.
      // If the conversion fails partway, roll back whatever links it wrote so the run is left as a
      // clean contiguous NoFatChain allocation (FAT entries cleared), not a NoFatChain stream carrying
      // stray chain links. file->noFatChain is still 1 here, so the returned state is self-consistent.
      if (convertToChain(vol, file->firstCluster, have) != 0) {
         for (uint32_t k = 0; k < have; k++) setFat(vol, file->firstCluster + k, 0);   // best-effort rollback
         file->allocClusters = have;
         return have;
      }
      file->noFatChain    = 0;
      file->cachedIndex   = 0;
      file->cachedCluster = file->firstCluster;
   }

   // Phase 2 - FAT-chain extension (fragmented fallback): only to `need`, no over-reservation
   // (chained clusters cost a FAT write each, so there's nothing to batch).
   //
   // Follow the FAT to the chain's TRUE end before appending, re-syncing `have` to the real length.
   // A foreign file may have more clusters allocated than ceil(size/cluster) (legal Windows
   // preallocation), so resuming by the size-derived index `have-1` could land MID-chain; chaining
   // a new cluster there would overwrite a live link and cross-link/orphan the rest. `have-1` would
   // also underflow when have == 0. Walking to EOC (resuming from the cluster cache, so a sequential
   // stream stays amortized O(1)) avoids both.
   uint32_t tail;
   if (have == 0) {
      tail = file->firstCluster;
      if (!isClusterValid(vol, tail)) { file->allocClusters = 0; return 0; }
      have = 1;
   } else {
      tail = getClusterAt(file, have - 1);
   }
   uint32_t walkGuard = 0;
   while (isClusterValid(vol, tail)) {
      uint32_t next = getNextCluster(vol, tail);
      if (next == 0) break;                       // reached EOC: tail is the real last cluster
      tail = next;
      have++;
      if (++walkGuard > vol->clusterCount) break; // corrupt/cyclic chain: stop
   }
   file->cachedIndex   = have - 1;
   file->cachedCluster = tail;

   while (have < need && isClusterValid(vol, tail)) {
      uint32_t nextCluster = allocCluster(vol);
      if (nextCluster == 0) break;
      if (chainCluster(vol, tail, nextCluster) != 0) { freeCluster(vol, nextCluster); break; }
      tail = nextCluster;
      have++;
   }
   file->allocClusters = have;
   return have;
}

static void releaseClustersFrom(ExfatFile *file, uint32_t used, uint32_t have);   // defined below

// Flushes a written file's new FirstCluster / DataLength and allocation flags into its
// on-disk entry set, then recomputes the SetChecksum. Returns 0 / -1.
static int flushEntry(ExfatFile *file)
{
   uint32_t clusterBytes   = getClusterBytes(file->vol);
   uint32_t used = (uint32_t)((file->size + clusterBytes - 1) / clusterBytes);

   // Record the entry's true size/allocation FIRST, then give back any clusters over-reserved
   // during writing but never filled. Ordering the entry write before the release means a failed
   // release leaves clusters merely leaked (allocated-but-unreferenced, fsck-recoverable) rather
   // than an entry that points at clusters the bitmap has already freed.
   uint8_t set[MAX_SET_ENTRIES * DIR_ENTRY_BYTES];
   if (readEntrySet(file->vol, &file->entry, set) != 0) return -1;

   uint32_t timestamp = getNowTimestamp();
   writeLe32(set + 12, timestamp);          // File: LastModifiedTimestamp - the data just changed
   writeLe32(set + 16, timestamp);          // File: LastAccessedTimestamp
   set[23] = EXFAT_TZ_UTC;      // LastModifiedUtcOffset (UTC)
   set[24] = EXFAT_TZ_UTC;      // LastAccessedUtcOffset (UTC)

   // ValidDataLength is the true valid high-water mark, which our writes keep == size but a foreign
   // file may have below size; writing file->size here would falsely declare an unwritten tail valid
   // and expose stale bytes. The write path zero-fills any gap before advancing validSize, so
   // [0, validSize) is always genuinely valid.
   uint64_t validOnDisk = file->validSize > file->size ? file->size : file->validSize;
   uint8_t *stream = set + DIR_ENTRY_BYTES;
   stream[1] = file->size ? (file->noFatChain ? STREAM_FLAGS_NEW : STREAM_FLAGS_CHAIN) : STREAM_FLAGS_EMPTY;
   writeLe64(stream + 8,  validOnDisk);                           // ValidDataLength
   writeLe32(stream + 20, file->size ? file->firstCluster : 0);   // FirstCluster
   writeLe64(stream + 24, file->size);                            // DataLength
   writeLe16(set + 2, computeChecksum(set, file->entry.count));
   if (writeEntrySet(file->vol, &file->entry, set) != 0) return -1;

   // the file now owns exactly ceil(size/clusterBytes) clusters; release the over-reserved tail
   if (file->allocClusters > used) {
      releaseClustersFrom(file, used, file->allocClusters);
      file->allocClusters = used;
   }
   return 0;
}

int truncateExfat(ExfatFile *file)
{
   if (!file->vol || !file->vol->mounted) return -1;
   ensureVolumeDirty(file->vol);

   // Remember the old allocation, zero the handle, then persist the now-empty entry set BEFORE
   // freeing the clusters. Writing the entry first means a crash/eject in the gap leaves the entry
   // pointing at nothing (recoverable lost clusters) rather than at clusters the bitmap has freed
   // and may have re-handed to another file. (Previously the entry rewrite was deferred to close,
   // which is skipped after a yank - so the freed clusters could be cross-linked.)
   uint32_t oldFirst      = file->firstCluster;
   int      oldNoFatChain = file->noFatChain;
   uint64_t oldSize       = file->size;

   file->firstCluster  = 0;
   file->size          = 0;
   file->validSize     = 0;
   file->position      = 0;
   file->noFatChain    = 0;
   file->cachedIndex   = 0;
   file->cachedCluster = 0;
   file->allocClusters = 0;
   file->dirty         = 1;
   if (flushEntry(file) != 0) return -1;   // persist size=0 / cluster=0 now, not at close
   file->dirty = 0;

   freeClusterChain(file->vol, oldFirst, oldNoFatChain, oldSize);
   return 0;
}

// Releases every cluster from file index `used` up to `have` (the count currently allocated),
// re-terminating the kept part. Used to give back clusters reserved for a write that then fell
// short (disk full or a mid-write I/O error), so the volume never leaks allocated-but-unowned
// space. A no-op when used == have (the normal full-write case).
static void releaseClustersFrom(ExfatFile *file, uint32_t used, uint32_t have)
{
   ExfatVolume *vol = file->vol;
   if (used >= have) return;
   if (file->noFatChain) {
      markClusterRun(vol, file->firstCluster + used, have - used, 0);
      if (file->firstCluster + used < vol->allocHint) vol->allocHint = file->firstCluster + used;
      invalidateDirEnd();
      if (used == 0) { file->firstCluster = 0; file->cachedIndex = 0; file->cachedCluster = 0; }
      return;
   }
   if (used == 0) {
      freeClusterChain(vol, file->firstCluster, 0, (uint64_t)have * getClusterBytes(vol));
      file->firstCluster = 0; file->cachedIndex = 0; file->cachedCluster = 0;
      return;
   }
   uint32_t last = getClusterAt(file, used - 1);          // last cluster we keep
   uint32_t c    = getNextCluster(vol, last);
   setFat(vol, last, EXFAT_EOC);
   while (isClusterValid(vol, c)) {
      uint32_t n = getNextCluster(vol, c);
      setFat(vol, c, 0); freeCluster(vol, c);
      c = n;
   }
   file->cachedIndex = used - 1; file->cachedCluster = last;
}

// Zeroes the on-disk byte range [from, to) of an open file (clusters already reserved). Honors
// exFAT's zero-tail rule: if a write begins past ValidDataLength (e.g. appending to a foreign file
// preallocated with ValidDataLength < DataLength), the skipped gap must read back as zero, not as
// the stale/deleted bytes physically present in those already-allocated clusters. Our own files
// keep validSize == size, so this never runs for them. Returns 0 / -1.
static int zeroFileRange(ExfatFile *file, uint64_t from, uint64_t to)
{
   ExfatVolume *vol = file->vol;
   uint32_t clusterBytes   = getClusterBytes(vol);
   uint32_t bytesPerSector = vol->bytesPerSector;
   uint32_t bounceSectors  = EXFAT_READ_BOUNCE / bytesPerSector;
   if (bounceSectors == 0) bounceSectors = 1;
   while (from < to) {
      uint32_t index       = (uint32_t)(from / clusterBytes);
      uint32_t offsetInClu = (uint32_t)(from % clusterBytes);
      uint32_t sectorInClu = offsetInClu / bytesPerSector;
      uint32_t offsetInSec = offsetInClu % bytesPerSector;
      uint32_t cluster = getClusterAt(file, index);
      if (!isClusterValid(vol, cluster)) return -1;
      uint64_t lba    = clusterToSector(vol, cluster) + sectorInClu;
      uint64_t remain = to - from;
      if (offsetInSec == 0 && remain >= bytesPerSector) {   // whole sectors: write zeros from the bounce
         uint32_t run = vol->sectorsPerCluster - sectorInClu;
         uint32_t maxRun = (uint32_t)(remain / bytesPerSector);
         if (run > maxRun)        run = maxRun;
         if (run > bounceSectors) run = bounceSectors;
         memSet(fileSector, 0, (int)(run * bytesPerSector));
         if (writeSectors(vol->storageHandle, lba, run, fileSector) != 0) return -1;
         from += (uint64_t)run * bytesPerSector;
      } else {                                              // partial edge sector: read-modify-write
         uint32_t chunk = bytesPerSector - offsetInSec;
         if ((uint64_t)chunk > remain) chunk = (uint32_t)remain;
         if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
         memSet(writeScratch + offsetInSec, 0, (int)chunk);
         if (writeSectors(vol->storageHandle, lba, 1, writeScratch) != 0) return -1;
         from += chunk;
      }
   }
   invalidateCaches();
   return 0;
}

int writeExfat(ExfatFile *file, const void *buffer, int length)
{
   if (length < 0) return -1;
   if (!file->writable) return -1;
   ExfatVolume *vol = file->vol;
   // vol is NULL after a yank (detachVolumeHandles); test it before dereferencing mounted so a late
   // write on a stale handle fails instead of crashing.
   if (!vol || !vol->mounted) return -1;
   if (file->appendMode) file->position = file->size;   // O_APPEND: every write lands at the current EOF
   ensureVolumeDirty(vol);                  // first write this session marks the volume dirty for the host

   const uint8_t *in = (const uint8_t *)buffer;
   uint32_t clusterBytes  = getClusterBytes(vol);
   uint32_t bytesPerSector = vol->bytesPerSector;

   // Reserve all clusters this write needs up front. A new/extending file becomes one
   // contiguous NoFatChain extent - no per-cluster FAT writes, the bitmap marked a sector at a
   // time - and the data then goes out in multi-cluster storage calls. A full volume caps it.
   uint64_t endPos = file->position + (uint64_t)length;
   uint32_t need   = (uint32_t)((endPos + clusterBytes - 1) / clusterBytes);
   uint32_t have   = reserveClusters(file, need);
   uint64_t capacity = (uint64_t)have * clusterBytes;
   uint64_t want = (uint64_t)length;
   if (file->position + want > capacity) want = (capacity > file->position) ? capacity - file->position : 0;

   // If this write starts beyond ValidDataLength, zero the skipped gap on disk first so it reads
   // back as zero (exFAT's zero-tail rule) instead of exposing stale cluster contents. No-op for
   // our own files (validSize == size) and for writes that start within the valid region.
   if (want > 0 && file->position > file->validSize) {
      if (zeroFileRange(file, file->validSize, file->position) != 0) return -1;
      file->validSize = file->position;
   }

   uint32_t bounceSectors = EXFAT_READ_BOUNCE / bytesPerSector;
   if (bounceSectors == 0) bounceSectors = 1;
   int written = 0;
   while ((uint64_t)written < want) {
      uint32_t index       = (uint32_t)(file->position / clusterBytes);
      uint32_t offsetInClu = (uint32_t)(file->position % clusterBytes);
      uint32_t sectorInClu = offsetInClu / bytesPerSector;
      uint32_t offsetInSec = offsetInClu % bytesPerSector;
      uint32_t remain      = (uint32_t)(want - (uint64_t)written);

      // Fast path: sector-aligned position with an aligned source and a full sector to write -
      // push as many physically-contiguous sectors as possible straight from the caller's
      // buffer (no bounce, spanning whole clusters) in one storage call.
      if (offsetInSec == 0 && remain >= bytesPerSector && isDmaAligned(in + written)) {
         uint32_t maxSectors = remain / bytesPerSector;
         uint64_t lba;
         uint32_t run = findContigSpan(file, index, sectorInClu, maxSectors, &lba);
         if (run == 0) break;
         if (writeSectors(vol->storageHandle, lba, run, in + written) != 0) break;
         uint32_t bytes = run * bytesPerSector;
         written += (int)bytes; file->position += bytes;
         if (file->position > file->size)      file->size      = file->position;
         if (file->position > file->validSize) file->validSize = file->position;
         continue;
      }

      // Slow path: partial edge sector, or an unaligned source buffer.
      uint32_t cluster = getClusterAt(file, index);
      if (!isClusterValid(vol, cluster)) break;
      uint64_t lba = clusterToSector(vol, cluster) + sectorInClu;
      if (offsetInSec == 0 && remain >= bytesPerSector) {
         // full sectors but unaligned source: bounce a run within this cluster
         uint32_t run = vol->sectorsPerCluster - sectorInClu;
         if (run > remain / bytesPerSector) run = remain / bytesPerSector;
         if (run > bounceSectors) run = bounceSectors;
         uint32_t bytes = run * bytesPerSector;
         memCopy(fileSector, in + written, (int)bytes);
         if (writeSectors(vol->storageHandle, lba, run, fileSector) != 0) break;
         written += (int)bytes; file->position += bytes;
      } else {
         // partial leading or trailing sector: read-modify-write a single sector
         uint32_t chunk = bytesPerSector - offsetInSec;
         if (chunk > remain) chunk = remain;
         if (readSectors(vol->storageHandle, lba, 1, writeScratch) != 0) break;
         memCopy(writeScratch + offsetInSec, in + written, (int)chunk);
         if (writeSectors(vol->storageHandle, lba, 1, writeScratch) != 0) break;
         written += (int)chunk; file->position += chunk;
      }
      if (file->position > file->size)      file->size      = file->position;
      if (file->position > file->validSize) file->validSize = file->position;
   }

   // Over-reserved clusters are kept across write calls (so a streaming copy doesn't re-touch
   // the bitmap each call) and released in flushEntry on close.
   if (written > 0) file->dirty = 1;
   return written > 0 ? written : (length == 0 ? 0 : -1);
}

// ============================================================================
// VFS backend
//
// Wraps the reader above as a VfsOps table and registers it with the VFS so
// consumers see exFAT volumes as /exfat<port> beside the cellFs devices.
// initVfs() calls initExfat(), so exFAT comes up wherever the VFS does. It is
// light and prx-safe (no libc), so riding into the plugins that link the VFS is fine.
//
// Every op takes one lock: the reader/writer share static sector buffers and the UI,
// folder-sizer and ftp threads all reach the filesystem concurrently, so exFAT access
// is serialized. Create, write, delete and rename are all supported.
// ============================================================================

#define EXFAT_MAX_VOLUMES    8    // USB ports 0-7; segment is "exfat<port>"
#define EXFAT_MAX_OPEN_DIRS  80   // folder-sizer alone nests up to 64 dirs
#define EXFAT_MAX_OPEN_FILES 16

static ExfatVolume   volumes[EXFAT_MAX_VOLUMES];
static ExfatDir      dirPool[EXFAT_MAX_OPEN_DIRS];
static uint8_t       dirUsed[EXFAT_MAX_OPEN_DIRS];
static ExfatFile     filePool[EXFAT_MAX_OPEN_FILES];
static uint8_t       fileUsed[EXFAT_MAX_OPEN_FILES];
static sys_lwmutex_t exfatLock;
static int           exfatLockReady;

// Frees the shared scratch block once no volume is mounted, so an idle system holds no exFAT heap.
// Called after any mount failure and after every unmount; a no-op while a volume is still mounted.
static void releaseScratchIfIdle(void)
{
   if (!scratchAddr) return;
   for (int i = 0; i < EXFAT_MAX_VOLUMES; i++) if (volumes[i].mounted) return;   // still in use
   sysMemFree(scratchAddr);
   scratchAddr = 0;
   dirSector = fatSector = writeScratch = bootSector = fileSector = 0;
   upcaseCp = upcaseUp = 0;
   upcaseTableEpoch = 0;   // force a reload on the next mount
}

// "exfat<port>" and its native prefix "exfat<port>:" (port is a single digit).
static void buildNames(int port, char *segment, char *native)
{
   const char *stem = "exfat";
   int i = 0;
   while (stem[i]) { segment[i] = stem[i]; i++; }
   segment[i++] = (char)('0' + port);
   segment[i]   = '\0';
   int j = 0;
   while (segment[j]) { native[j] = segment[j]; j++; }
   native[j++] = ':';
   native[j]   = '\0';
}

// maps a native path ("exfat<port>:/in/path") to its volume and in-volume path.
static ExfatVolume *volumeFromNative(const char *native, const char **inPath)
{
   const char *colon = native;
   while (*colon && *colon != ':') colon++;
   if (*colon != ':') return 0;
   *inPath = colon + 1;   // always begins with '/' (resolvePath joins one in)

   const char *digits = native + 5;   // skip "exfat"
   if (digits >= colon) return 0;
   // Parse unsigned and bound the running value each step: a long all-digit string can no longer
   // overflow a signed int (UB) before the range check - it is rejected the moment it exceeds the
   // port range. (The native prefix is normally built by buildNames as a single digit; this guards
   // a hostile/oversized path that reaches the router.)
   unsigned port = 0;
   for (const char *d = digits; d < colon; d++) {
      if (*d < '0' || *d > '9') return 0;
      port = port * 10u + (unsigned)(*d - '0');
      if (port >= EXFAT_MAX_VOLUMES) return 0;   // out of range (and caps the value: never overflows)
   }
   if (!volumes[port].mounted) return 0;
   return &volumes[port];
}

static int allocDirSlot(void)
{
   for (int i = 0; i < EXFAT_MAX_OPEN_DIRS; i++)
      if (!dirUsed[i]) { dirUsed[i] = 1; return i; }
   return -1;
}

static int allocFileSlot(void)
{
   for (int i = 0; i < EXFAT_MAX_OPEN_FILES; i++)
      if (!fileUsed[i]) { fileUsed[i] = 1; return i; }
   return -1;
}

// Neutralizes any pooled file/dir handles still bound to a volume that is being unmounted (device
// removed). Their slots stay reserved until the app closes them, but clearing dirty + nulling the
// volume pointer means a late close/fsync can never flush this handle's stale size/cluster fields
// into whatever volume later re-mounts into the same slot. Caller holds exfatLock.
static void detachVolumeHandles(const ExfatVolume *vol)
{
   for (int i = 0; i < EXFAT_MAX_OPEN_FILES; i++) {
      if (fileUsed[i] && filePool[i].vol == vol) { filePool[i].dirty = 0; filePool[i].vol = 0; }
   }
   for (int i = 0; i < EXFAT_MAX_OPEN_DIRS; i++) {
      if (dirUsed[i] && dirPool[i].vol == vol) dirPool[i].vol = 0;
   }
}

// Repoints every open file handle on `vol` whose entry set sits at `from` to `to`. Used after a
// rename relocates an entry set, so a still-open handle flushes into the live set rather than the
// cleared source slot (which would resurrect/cross-link a directory entry). Caller holds exfatLock.
static void repointOpenHandles(const ExfatVolume *vol, const ExfatEntryLoc *from, const ExfatEntryLoc *to)
{
   for (int i = 0; i < EXFAT_MAX_OPEN_FILES; i++) {
      if (!fileUsed[i] || filePool[i].vol != vol) continue;
      ExfatEntryLoc *entry = &filePool[i].entry;
      if (entry->cluster == from->cluster && entry->sectorInClu == from->sectorInClu &&
          entry->entryInSector == from->entryInSector)
         *entry = *to;   // copies cluster/sector/entry + the new set's count and chain mode
   }
}

static int statExfatOp(const char *native, VfsStat *outStat)
{
   lock(&exfatLock);
   const char *inPath;
   ExfatVolume *vol = volumeFromNative(native, &inPath);
   ExfatInfo info;
   int result = (vol && statExfat(vol, inPath, &info) == 0) ? 0 : -1;
   if (result == 0) {
      outStat->size  = info.size;
      outStat->mtime = info.mtime;
      outStat->isDir = info.isDir;
      outStat->attributes = 0;   // exFAT attribute mapping not surfaced here (NTFS feature); keep the field initialized
      // exFAT has no POSIX permissions; synthesize a plain mode (writes are gated by the VFS ops).
      outStat->mode  = info.isDir ? (0040000u | 0555u) : (0100000u | 0444u);
   }
   unlock(&exfatLock);
   return result;
}

static int getFreeExfatOp(const char *native, uint64_t *freeBytes, uint64_t *totalBytes)
{
   lock(&exfatLock);
   const char *inPath;
   ExfatVolume *vol = volumeFromNative(native, &inPath);
   int result = vol ? getExfatFree(vol, freeBytes, totalBytes) : -1;
   unlock(&exfatLock);
   return result;
}

static int openExfatDirOp(const char *native, VfsDir *dir)
{
   lock(&exfatLock);
   const char *inPath;
   ExfatVolume *vol = volumeFromNative(native, &inPath);
   ExfatInfo info;
   int slot = -1;
   if (vol && statExfat(vol, inPath, &info) == 0 && info.isDir)
      slot = allocDirSlot();
   if (slot >= 0)
      openExfatDir(&dirPool[slot], vol, info.firstCluster, info.noFatChain, info.size);
   dir->descriptor   = slot;
   dir->nativeHandle = 0;
   unlock(&exfatLock);
   return slot >= 0 ? 0 : -1;
}

static int readExfatDirOp(VfsDir *dir, char *nameOut, int nameCapacity, VfsEntryType *typeOut)
{
   lock(&exfatLock);
   ExfatInfo info;
   int result = 0;
   if (dir->descriptor >= 0 && dir->descriptor < EXFAT_MAX_OPEN_DIRS) {
      ExfatDir *ed = &dirPool[dir->descriptor];
      result = readExfatDir(ed, nameOut, nameCapacity, &info);
      if (result == 1 && typeOut) *typeOut = info.isDir ? VFS_ENTRY_DIR : VFS_ENTRY_FILE;  // exFAT has no symlinks
      // surface a mid-walk I/O fault as -1 (readExfatDir reports it as end-of-dir for the
      // internal lookups, but the VFS contract needs error != end so the tree walkers abort
      // instead of treating a partial listing as a clean, complete walk).
      if (result == 0 && ed->ioError) result = -1;
   }
   unlock(&exfatLock);
   return result;
}

static void closeExfatDirOp(VfsDir *dir)
{
   lock(&exfatLock);
   if (dir->descriptor >= 0 && dir->descriptor < EXFAT_MAX_OPEN_DIRS) {
      closeExfatDir(&dirPool[dir->descriptor]);
      dirUsed[dir->descriptor] = 0;
   }
   dir->descriptor = -1;
   unlock(&exfatLock);
}

static int openExfatOp(const char *native, int flags, VfsFile *file)
{
   int writing = (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) != 0;

   lock(&exfatLock);
   const char *inPath;
   ExfatVolume *vol = volumeFromNative(native, &inPath);
   int slot = -1;
   if (vol) {
      // One lookup opens the file, creating it first if O_CREAT and it's absent (an
      // existing one is fine to open and write). This avoids a second directory scan.
      int create = writing && (flags & VFS_O_CREAT);
      slot = allocFileSlot();
      if (slot >= 0 && openOrCreateExfat(&filePool[slot], vol, inPath, create) != 0) {
         fileUsed[slot] = 0;
         slot = -1;
      }
      if (slot >= 0 && writing) {
         filePool[slot].writable   = 1;
         filePool[slot].appendMode = (flags & VFS_O_APPEND) ? 1 : 0;   // writes reposition to EOF (POSIX append)
         if (flags & VFS_O_TRUNC) {
            if (truncateExfat(&filePool[slot]) != 0) {   // truncation failed (I/O): don't open over leaked clusters
               closeExfat(&filePool[slot]);
               fileUsed[slot] = 0;
               slot = -1;
            }
         } else if (flags & VFS_O_APPEND) seekExfat(&filePool[slot], filePool[slot].size);
      }
   }
   file->descriptor = slot;
   unlock(&exfatLock);
   return slot >= 0 ? 0 : -1;
}

static int64_t readExfatOp(VfsFile *file, void *buffer, uint64_t length)
{
   if (file->descriptor < 0 || file->descriptor >= EXFAT_MAX_OPEN_FILES) return -1;
   lock(&exfatLock);
   int chunk = length > 0x7FFFFFFF ? 0x7FFFFFFF : (int)length;
   int result = readExfat(&filePool[file->descriptor], buffer, chunk);
   unlock(&exfatLock);
   return result < 0 ? -1 : (int64_t)result;
}

static int64_t seekExfatOp(VfsFile *file, int64_t offset, int whence)
{
   if (file->descriptor < 0 || file->descriptor >= EXFAT_MAX_OPEN_FILES) return -1;
   if (whence != VFS_SEEK_SET && whence != VFS_SEEK_CUR && whence != VFS_SEEK_END) return -1;   // reject unknown whence (matches cellFs), no silent SEEK_SET
   lock(&exfatLock);
   ExfatFile *handle = &filePool[file->descriptor];
   int64_t base = (whence == VFS_SEEK_CUR) ? (int64_t)handle->position
               : (whence == VFS_SEEK_END) ? (int64_t)handle->size : 0;
   int64_t target = base + offset;
   if (target < 0) target = 0;
   seekExfat(handle, (uint64_t)target);
   int64_t position = (int64_t)handle->position;
   unlock(&exfatLock);
   return position;
}

// true if any still-open pooled handle on `vol` is writable. Caller holds exfatLock.
static int hasOpenWriter(const ExfatVolume *vol)
{
   for (int i = 0; i < EXFAT_MAX_OPEN_FILES; i++)
      if (fileUsed[i] && filePool[i].vol == vol && filePool[i].writable) return 1;
   return 0;
}

static int closeExfatOp(VfsFile *file)
{
   int result = 0;
   lock(&exfatLock);
   if (file->descriptor >= 0 && file->descriptor < EXFAT_MAX_OPEN_FILES) {
      ExfatVolume *vol = filePool[file->descriptor].vol;  // capture before closeExfat nulls file->vol
      result = closeExfat(&filePool[file->descriptor]);   // commit error surfaces to closeFs
      fileUsed[file->descriptor] = 0;                     // this slot now excluded from the scan below

      // The volume is consistent on disk once the last writable handle closes (write-through data +
      // flushEntry just ran). Clear the host's VolumeDirty hint now, while the device is still
      // present - the FTP "transfer then yank" path never reaches a present-time unmount, so the
      // unmount-time clear (see unmountExfat) can't run for it. The next write re-arms the flag via
      // ensureVolumeDirty. exfatLock serializes open/write/close, so this scan can't race a writer.
      if (result == 0 && vol && vol->mounted && vol->volumeDirty && !hasOpenWriter(vol)) {
         if (setVolumeDirty(vol, 0) == 0) vol->volumeDirty = 0;
      }
   }
   file->descriptor = -1;
   unlock(&exfatLock);
   return result;
}

// rename/move within one volume; cross-volume is refused (the VFS does copy+delete).
static int renameExfatOp(const char *from, const char *to)
{
   lock(&exfatLock);
   const char *fromIn, *toIn;
   ExfatVolume *fromVol = volumeFromNative(from, &fromIn);
   ExfatVolume *toVol   = volumeFromNative(to, &toIn);
   int result = (fromVol && fromVol == toVol) ? renameExfatPath(fromVol, fromIn, toIn) : -1;
   unlock(&exfatLock);
   return result;
}

// Creates a directory. Maps "already a directory" (-2) to success so the call is
// idempotent, matching makeDirPath's "0 if created or already present" contract.
static int mkdirExfatOp(const char *native)
{
   lock(&exfatLock);
   const char *inPath;
   ExfatVolume *vol = volumeFromNative(native, &inPath);
   int result = vol ? mkdirExfatPath(vol, inPath) : -1;
   unlock(&exfatLock);
   return (result == 0 || result == -2) ? 0 : -1;
}
// Removes a file. Maps "already absent" (-2) to success so removeFilePath/deleteFile are
// idempotent ("0 if already absent"), matching the cellFs backend behind the same vtable slot.
static int rmfileExfatOp(const char *native)
{
   lock(&exfatLock);
   const char *inPath;
   ExfatVolume *vol = volumeFromNative(native, &inPath);
   int result = vol ? unlinkExfatPath(vol, inPath) : -1;
   unlock(&exfatLock);
   return (result == 0 || result == -2) ? 0 : -1;
}
// Removes an empty directory. Maps "already absent" (-2) to success for idempotency.
static int rmdirExfatOp(const char *native)
{
   lock(&exfatLock);
   const char *inPath;
   ExfatVolume *vol = volumeFromNative(native, &inPath);
   int result = vol ? rmdirExfatPath(vol, inPath) : -1;
   unlock(&exfatLock);
   return (result == 0 || result == -2) ? 0 : -1;
}
static int64_t writeExfatOp(VfsFile *file, const void *buffer, uint64_t length)
{
   if (file->descriptor < 0 || file->descriptor >= EXFAT_MAX_OPEN_FILES) return -1;
   lock(&exfatLock);
   int chunk  = length > 0x7FFFFFFF ? 0x7FFFFFFF : (int)length;
   int result = writeExfat(&filePool[file->descriptor], buffer, chunk);
   unlock(&exfatLock);
   return result < 0 ? -1 : (int64_t)result;
}

static int fsyncExfatOp(VfsFile *file)
{
   if (file->descriptor < 0 || file->descriptor >= EXFAT_MAX_OPEN_FILES) return -1;
   lock(&exfatLock);
   ExfatFile *handle = &filePool[file->descriptor];
   int result = (handle->dirty && handle->vol && handle->vol->mounted) ? flushEntry(handle) : 0;
   if (result == 0) handle->dirty = 0;
   unlock(&exfatLock);
   return result;
}

static const VfsOps exfatOps = {
   statExfatOp, renameExfatOp, mkdirExfatOp, rmfileExfatOp, rmdirExfatOp, getFreeExfatOp,
   openExfatDirOp, readExfatDirOp, closeExfatDirOp,
   openExfatOp, readExfatOp, writeExfatOp, seekExfatOp, fsyncExfatOp, closeExfatOp,
};

// Chooses the display/route segment for a freshly-mounted volume: its volume
// label when present, sanitized and not clashing with another mounted volume's
// segment; otherwise "exfat<port>". The native prefix stays "exfat<port>:" so
// the port is always recoverable from a routed path. caller holds exfatLock.
static void chooseSegment(int port, char *out, int cap)
{
   const char *label = volumes[port].label;
   int n = 0;
   for (int i = 0; label[i] && n < cap - 1; i++) {
      char c = label[i];
      if (c == '/' || c == '\\' || (unsigned char)c < 0x20) continue;   // not path-safe
      out[n++] = c;
   }
   out[n] = 0;

   int reject = (n == 0);
   for (int p = 0; p < EXFAT_MAX_VOLUMES && !reject; p++) {
      if (p == port || !volumes[p].mounted) continue;
      if (strEq(volumes[p].segment, out)) reject = 1;   // duplicate label -> fall back
   }
   if (reject) {
      char native[16];
      buildNames(port, out, native);   // out := "exfat<port>"
   }
}

// VFS backend hook: the VFS has found a device present on `port` and offers it to us. Try to mount
// it as exFAT. Returns VFS_PROBE_MOUNTED (it's ours, published), VFS_PROBE_NOT_MINE (read OK but
// not exFAT - let cellFs/another backend have it), or VFS_PROBE_NOT_READY (device not readable yet
// - the VFS retries). The VFS owns presence detection, so this never polls or re-probes.
static VfsProbeResult probeExfat(int port)
{
   lock(&exfatLock);
   VfsProbeResult result;
   int rc = mountExfat(&volumes[port], port);
   if (rc == EXFAT_MOUNT_OK) {
      char segment[16], native[16];
      buildNames(port, segment, native);                 // native := "exfat<port>:"
      chooseSegment(port, volumes[port].segment, (int)sizeof(volumes[port].segment));
      addVfsMount(volumes[port].segment, native, volumes[port].label, VFS_SCHEME_EXFAT, &exfatOps);
      result = VFS_PROBE_MOUNTED;
   } else if (rc == EXFAT_MOUNT_NOT_EXFAT) {
      result = VFS_PROBE_NOT_MINE;
   } else {
      result = VFS_PROBE_NOT_READY;
   }
   unlock(&exfatLock);
   return result;
}

// VFS backend hook: the device this backend mounted on `port` was removed; withdraw it.
static void releaseExfat(int port)
{
   lock(&exfatLock);
   if (port >= 0 && port < EXFAT_MAX_VOLUMES && volumes[port].mounted) {
      removeVfsMount(volumes[port].segment);
      detachVolumeHandles(&volumes[port]);   // stop any still-open handle from flushing into a future mount
      unmountExfat(&volumes[port]);
   }
   unlock(&exfatLock);
}

static void shutdownExfatBackend(void)
{
   lock(&exfatLock);
   for (int port = 0; port < EXFAT_MAX_VOLUMES; port++) {
      if (!volumes[port].mounted) continue;
      removeVfsMount(volumes[port].segment);
      detachVolumeHandles(&volumes[port]);
      unmountExfat(&volumes[port]);
   }
   unlock(&exfatLock);
}

void initExfat(void)
{
   if (!exfatLockReady) {
      createLock(&exfatLock);
      exfatLockReady = 1;
   }
   registerVfsBackend(probeExfat, releaseExfat, shutdownExfatBackend);   // VFS drives hotplug
}
