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
