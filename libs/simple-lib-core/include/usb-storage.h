#pragma once

// usb-storage - the device layer shared by the VFS and the filesystem-format backends.
//
// "Is a USB mass-storage device present on port N, and what is it?" is a device-level fact,
// independent of the filesystem on it (exFAT, NTFS, FAT32...). The VFS owns hotplug detection
// using these helpers and offers each newly-present device to the format backends in turn; a
// backend (exfat.c, later ntfs) only decides whether the device is its format. Keeping these
// here - rather than inside one backend - is what lets the VFS treat every format uniformly.
//
// Header-only (static inline) so it links into core, the app and the prx plugins without a
// separate translation unit. Storage I/O goes through simple-lib-core's scCall trampolines.

#include <stdint.h>
#include "syscall.h"

#define USB_STORAGE_MAX_PORTS  8     // lv2 exposes USB mass-storage on ports 0..7
#define STORAGE_GET_INFO       609   // sys_storage_get_device_info

// USB mass-storage device id for a port (0-5 and 6+ use different bases) - from
// apps/ManaGunZ/MGZ/source/exFAT.h USB_MASS_STORAGE().
static inline uint64_t getUsbDeviceId(int port)
{
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
