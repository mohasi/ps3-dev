// disc-dump - raw sector copy of the Blu-ray drive to an .iso (see disc-dump.h).
#include "disc-dump.h"
#include "storage-device.h"
#include "file-task.h"
#include "vfs.h"
#include "sfo.h"
#include "path.h"
#include "format.h"
#include "string-utilities.h"
#include "dbg.h"
#include <stdlib.h>   // memalign: the read buffer must be aligned for the storage syscall
#include <stdio.h>

#define DUMP_DIRECTORY     "/dev_hdd0/dumps"
#define DUMP_VOLUME        "/dev_hdd0"
#define DISC_SFO_PATH      "/dev_bdvd/PS3_GAME/PARAM.SFO"
#define SECTORS_PER_READ   0x400                                  // 2 MB per lv2 read
#define DUMP_BUFFER_BYTES  (SECTORS_PER_READ * BD_SECTOR_SIZE)
#define READ_ATTEMPTS      3
#define BUFFER_ALIGNMENT   128
#define MAX_NAME_ATTEMPTS  99                                      // "NAME (2).iso" ... "NAME (99).iso"

static char destinationPath[MAX_PATH_LEN];
static char statusMessage[MAX_PATH_LEN + 64];
static int  dumpFailed;

// the drive reports a disc, and how big it is. 0 when the tray is empty or unreadable.
static int getDiscInfo(StorageDeviceInfo *info)
{
   if (getStorageInfo(BD_DRIVE_DEVICE_ID, info) != 0) return 0;
   return info->sectorCount > 0 && info->sectorSize > 0;
}

int isDiscInDrive(void)
{
   StorageDeviceInfo info;
   return getDiscInfo(&info);
}

const char *getDiscDumpDestination(void) { return destinationPath; }
int         discDumpHadError(void)       { return dumpFailed; }
const char *getDiscDumpStatus(void)      { return statusMessage; }

// PS3 game discs name themselves in PARAM.SFO; PS2, video and data discs have none, so
// they fall back to a generic name.
static void getDiscName(char *out, int cap)
{
   char sfo[8192];
   int length = readFile(DISC_SFO_PATH, sfo, sizeof sfo);
   if (length > 0 && getSfoValue((const uint8_t *)sfo, (uint64_t)length, "TITLE_ID", out, cap) > 0 && out[0]) return;
   strCopy(out, cap, "disc");
}

// first free "<name>.iso" in the dump directory, so a second dump of the same disc never
// silently overwrites the first.
static void chooseDestination(const char *name)
{
   snprintf(destinationPath, sizeof destinationPath, "%s/%s.iso", DUMP_DIRECTORY, name);
   for (int attempt = 2; attempt <= MAX_NAME_ATTEMPTS && fileExists(destinationPath); attempt++)
      snprintf(destinationPath, sizeof destinationPath, "%s/%s (%d).iso", DUMP_DIRECTORY, name, attempt);
}

int prepareDiscDump(char *reasonOut, int cap)
{
   StorageDeviceInfo info;
   if (!getDiscInfo(&info)) {
      strCopy(reasonOut, cap, "There is no disc in the drive.");
      return -1;
   }

   uint64_t discBytes = info.sectorCount * (uint64_t)info.sectorSize;
   uint64_t freeBytes = 0, totalBytes = 0;
   if (getFreeSpace(DUMP_VOLUME, &freeBytes, &totalBytes) == 0 && freeBytes < discBytes) {
      char needed[24], available[24];
      formatSize(discBytes, needed);
      formatSize(freeBytes, available);
      snprintf(reasonOut, cap, "The disc needs %s but only %s is free.", needed, available);
      return -1;
   }

   char name[16];
   getDiscName(name, sizeof name);
   chooseDestination(name);
   logInfo("[dump] %s, %llu sectors of %u bytes -> %s\n", name, (unsigned long long)info.sectorCount,
           info.sectorSize, destinationPath);
   return 0;
}

// one chunk, retried. a chunk that will not read as a whole is retried sector by sector so a
// single bad spot costs only its own sectors; those that never read are left zeroed (the image
// keeps its length) and counted by the caller. returns the number of unreadable sectors, or -1
// if the user cancelled mid-retry.
static int readChunk(int driveHandle, uint64_t sector, uint32_t count, uint8_t *buffer)
{
   uint32_t sectorsRead = 0;
   for (int attempt = 0; attempt < READ_ATTEMPTS; attempt++) {
      if (readStorageRaw(driveHandle, sector, count, buffer, &sectorsRead) == 0 && sectorsRead == count) return 0;
      if (isCancelRequested()) return -1;
   }

   logWarn("[dump] chunk at sector %llu unreadable, falling back to single sectors\n", (unsigned long long)sector);
   int unreadable = 0;
   for (uint32_t index = 0; index < count; index++) {
      uint8_t *slot = buffer + (uint64_t)index * BD_SECTOR_SIZE;
      int ok = 0;
      for (int attempt = 0; attempt < READ_ATTEMPTS && !ok; attempt++) {
         ok = readStorageRaw(driveHandle, sector + index, 1, slot, &sectorsRead) == 0 && sectorsRead == 1;
         if (!ok && isCancelRequested()) return -1;
      }
      if (!ok) {
         memSet(slot, 0, BD_SECTOR_SIZE);
         unreadable++;
      }
   }
   return unreadable;
}

void runDiscDump(void)
{
   dumpFailed = 1;   // cleared once the whole disc is written
   strCopy(statusMessage, sizeof statusMessage, "The disc could not be read.");

   // open the drive and the destination
   StorageDeviceInfo info;
   if (!getDiscInfo(&info)) return;
   setTotalBytes(info.sectorCount * (uint64_t)info.sectorSize);

   int driveHandle = -1;
   if (openStorage(BD_DRIVE_DEVICE_ID, &driveHandle) != 0) {
      logError("[dump] could not open the Blu-ray drive\n");
      return;
   }

   uint8_t *buffer = (uint8_t *)memalign(BUFFER_ALIGNMENT, DUMP_BUFFER_BYTES);
   VfsFile  image;
   int      imageOpen          = 0;
   int      removePartialImage = 0;
   uint64_t unreadableSectors  = 0;
   uint64_t sector             = 0;
   if (!buffer) { logError("[dump] out of memory for the read buffer\n"); goto cleanup; }

   makeDirPath(DUMP_DIRECTORY);
   if (openFs(destinationPath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &image) != 0) {
      logError("[dump] could not create %s\n", destinationPath);
      strCopy(statusMessage, sizeof statusMessage, "The image file could not be created.");
      goto cleanup;
   }
   imageOpen = 1;

   // copy the disc. anything that stops it early leaves no image behind - a partial file
   // that looks like a dump is worse than none.
   while (sector < info.sectorCount) {
      uint32_t count = SECTORS_PER_READ;
      if (info.sectorCount - sector < count) count = (uint32_t)(info.sectorCount - sector);

      int unreadable = isCancelRequested() ? -1 : readChunk(driveHandle, sector, count, buffer);
      if (unreadable < 0) {
         strCopy(statusMessage, sizeof statusMessage, "Cancelled.");
         removePartialImage = 1;
         goto cleanup;
      }
      unreadableSectors += (uint64_t)unreadable;

      uint64_t chunkBytes = (uint64_t)count * BD_SECTOR_SIZE;
      if (writeFs(&image, buffer, chunkBytes) != (int64_t)chunkBytes) {
         logError("[dump] write failed at sector %llu\n", (unsigned long long)sector);
         strCopy(statusMessage, sizeof statusMessage, "Writing to the internal drive failed.");
         removePartialImage = 1;
         goto cleanup;
      }
      addProcessedBytes(chunkBytes);
      sector += count;
   }

   // report the outcome
   if (unreadableSectors > 0) {
      snprintf(statusMessage, sizeof statusMessage, "%llu sectors could not be read - the image is incomplete.",
               (unsigned long long)unreadableSectors);
      logError("[dump] finished with %llu unreadable sectors\n", (unsigned long long)unreadableSectors);
   } else {
      snprintf(statusMessage, sizeof statusMessage, "Dumped to %s", destinationPath);
      logInfo("[dump] finished, %llu sectors\n", (unsigned long long)info.sectorCount);
      dumpFailed = 0;
   }

cleanup:
   if (imageOpen) closeFs(&image);
   if (removePartialImage) deleteFile(destinationPath);
   free(buffer);
   closeStorage(driveHandle);
}
