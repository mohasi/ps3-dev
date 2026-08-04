#pragma once

// storage-device - the lv2 storage layer shared by the VFS, the filesystem-format backends
// and the disc dumper.
//
// "Is a device present, what is it, and give me its raw sectors" is device-level and
// independent of the filesystem on it (exFAT, NTFS, FAT32...). The VFS owns USB hotplug
// detection using these helpers and offers each newly-present device to the format backends
// in turn; a backend (exfat.c, ntfs.c) only decides whether the device is its format. The
// Blu-ray drive is the same kind of device with a fixed id, read the same way.
//
// Header-only (static inline) so it links into core, the app and the prx plugins without a
// separate translation unit. Storage I/O goes through simple-lib-core's scCall trampolines.
//
// syscall numbers and argument order are taken verbatim from the in-tree references
// (hb-samples/ManaGunZ/MGZ/source/bd/storage.h, apps/xai_plugin functions.cpp).

#include <stdint.h>
#include "syscall.h"
#include "string-utilities.h"   // memSet - prx-safe zero (a raw loop lowers to libc memset, which won't resolve in a vsh plugin)

#define USB_STORAGE_MAX_PORTS  8     // lv2 exposes USB mass-storage on ports 0..7
#define STORAGE_OPEN           600   // sys_storage_open
#define STORAGE_CLOSE          601   // sys_storage_close
#define STORAGE_READ           602   // sys_storage_read
#define STORAGE_GET_INFO       609   // sys_storage_get_device_info

#define BD_DRIVE_DEVICE_ID     0x0101000000000006ULL   // the Blu-ray drive
#define BD_SECTOR_SIZE         2048

// USB mass-storage device id for a port (0-5 and 6+ use different bases) - from
// apps/ManaGunZ/MGZ/source/exFAT.h USB_MASS_STORAGE(). port is clamped to the
// lv2 contract [0, USB_STORAGE_MAX_PORTS) so an out-of-range value can't fabricate
// a device id that aliases a real device's id space.
static inline uint64_t getUsbDeviceId(int port)
{
   if (port < 0) port = 0;
   if (port >= USB_STORAGE_MAX_PORTS) port = USB_STORAGE_MAX_PORTS - 1;
   return port < 6 ? 0x10300000000000AULL + (uint64_t)port
                   : 0x10300000000001FULL + (uint64_t)(port - 6);
}

// lv2 storage device info; layout mirrors sys_device_info_t (ManaGunZ storage.h).
typedef struct {
   char     label[32];
   uint32_t reserved1;
   uint32_t reserved2;
   uint64_t sectorCount;
   uint32_t sectorSize;
   uint32_t reserved3;
   uint8_t  reserved4[8];
} __attribute__((packed)) StorageDeviceInfo;

// Queries a device by id. Returns 0 on success (device present), non-zero otherwise.
static inline int getStorageInfo(uint64_t deviceId, StorageDeviceInfo *info)
{
   return (int)scCall2(STORAGE_GET_INFO, deviceId, (uint64_t)(uintptr_t)info);
}

// True if a USB mass-storage device is present on `port`. Non-DMA (no sector read), so it
// neither blinks the USB LED nor touches the data path - safe to poll for hotplug.
static inline int isUsbDevicePresent(int port)
{
   StorageDeviceInfo info;
   return getStorageInfo(getUsbDeviceId(port), &info) == 0;
}

static inline int openStorage(uint64_t deviceId, int *outStorageHandle)
{
   return (int)scCall4(STORAGE_OPEN, deviceId, 0, (uint64_t)(uintptr_t)outStorageHandle, 0);
}

static inline int closeStorage(int storageHandle)
{
   return (int)scCall1(STORAGE_CLOSE, (uint64_t)storageHandle);
}

// One raw sector read. `buffer` should be at least 32-byte aligned; outRead receives the
// sector count actually transferred. Returns 0 on success, an lv2 error otherwise.
static inline int readStorageRaw(int storageHandle, uint64_t sector, uint32_t count, void *buffer, uint32_t *outRead)
{
   return (int)scCall7(STORAGE_READ, (uint64_t)storageHandle, 0, sector, count,
                       (uint64_t)(uintptr_t)buffer, (uint64_t)(uintptr_t)outRead, 0);
}

// STORAGE_READ (602) only serves media the XMB authenticated. A PS2 or video disc is refused with
// ENXIO (0x8001002f) even though lv2 mounts and reads that same disc's files - measured on 4.93,
// and MultiMAN's "Create ISO" fails there identically. Sending the drive its own ATAPI read
// command instead was tried and refused too, so there is no sector-level route to those discs.
#define STORAGE_SEND_DEVICE_CMD   604    // sys_storage_send_device_cmd

// --- disc table of contents (READ TOC) ---
// Reads the disc's track layout via the native SCSI READ TOC command. Generic drive knowledge:
// an audio-CD ripper uses the per-track start LBAs for track boundaries, and a CDDB lookup adds
// the 150-frame lead-in to each to get its frame offsets. Mirrors webMAN cobra/scsi.h.
#define SCSI_READ_TOC        0x43   // MMC READ TOC/PMA/ATIP
#define STORAGE_CMD_NATIVE   0x01   // send-device-cmd subcommand: a native SCSI CDB (not the ATAPI packet form)
#define DISC_TRACK_LEADOUT   0xAA   // the lead-out is reported as track number 0xAA

// SCSI READ TOC command block at offset 0, plus the storage-command envelope at offset 32 -
// together the 56-byte command buffer the device command expects.
typedef struct {
   uint8_t  opcode, rvMsf, rvFormat, reserved[3], trackSessionNum;
   uint16_t allocLength;   // SCSI is big-endian; a native u16 on PPC is already big-endian
   uint8_t  control;
} __attribute__((packed)) ScsiReadTocCmd;

typedef struct { uint32_t inlen, unk1, outlen, unk2, unk3; } __attribute__((packed)) StorageScsiEnvelope;

// Reads the TOC from an already-open drive handle. trackStartLba[i] receives the start LBA of
// track i+1 (raw drive LBA, no lead-in); *leadoutLba receives the lead-out start LBA. Returns the
// highest track number found (>=1), or -2 on a device-command failure. trackStartLba is zeroed
// first, so tracks past the count stay 0. Red Book CDs number tracks contiguously from 1.
static inline int readDiscToc(int storageHandle, uint32_t *trackStartLba, int maxTracks, uint32_t *leadoutLba)
{
   *leadoutLba = 0;
   memSet(trackStartLba, 0, maxTracks * (int)sizeof trackStartLba[0]);

   unsigned char scsiCmd[56];
   memSet(scsiCmd, 0, sizeof scsiCmd);
   ScsiReadTocCmd      *cmd = (ScsiReadTocCmd *)scsiCmd;
   StorageScsiEnvelope *env = (StorageScsiEnvelope *)(scsiCmd + 32);
   unsigned char toc[4 + 100 * 8];   // 4-byte header + up to 100 descriptors (99 tracks + lead-out)
   cmd->opcode      = SCSI_READ_TOC;
   cmd->allocLength = (uint16_t)sizeof toc;
   env->inlen  = 12;
   env->unk1   = env->unk2 = env->unk3 = 1;
   env->outlen = sizeof toc;

   if ((int)scCall6(STORAGE_SEND_DEVICE_CMD, (uint64_t)storageHandle, STORAGE_CMD_NATIVE,
                    (uint64_t)(uintptr_t)scsiCmd, sizeof scsiCmd, (uint64_t)(uintptr_t)toc, sizeof toc) != 0)
      return -2;

   // response: [u16 toc_length][u8 first][u8 last][ descriptors: rsv, adr/ctl, track#, rsv, u32 lba ]
   int tocLength   = (toc[0] << 8) | toc[1];   // bytes following this 2-byte length
   int descriptors = (tocLength - 2) / 8;
   if (descriptors < 0)   descriptors = 0;
   if (descriptors > 100) descriptors = 100;

   int count = 0;
   for (int d = 0; d < descriptors; d++) {
      const unsigned char *entry = toc + 4 + d * 8;
      int track    = entry[2];
      uint32_t lba = ((uint32_t)entry[4] << 24) | ((uint32_t)entry[5] << 16) | ((uint32_t)entry[6] << 8) | entry[7];
      if (track == DISC_TRACK_LEADOUT) *leadoutLba = lba;
      else if (track >= 1 && track <= maxTracks) {
         trackStartLba[track - 1] = lba;
         if (track > count) count = track;
      }
   }
   return count;
}
