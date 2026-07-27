#include "toc.h"
#include "storage-device.h"   // openStorage/closeStorage/readDiscToc/BD_DRIVE_DEVICE_ID
#include "dbg.h"

#define TAG "[cdi] "

#define CD_LEAD_IN_FRAMES  150   // a CDDB/EAC frame offset is the drive LBA plus the 150-frame lead-in

// read the audio CD's track layout and hand back CDDB frame offsets (drive LBA + 150). The raw
// TOC read is the shared drive helper; the +150 lead-in convention is the CDDB-specific part.
int readCdToc(uint32_t *frameOffsets, int maxTracks, uint32_t *leadoutFrame)
{
   *leadoutFrame = 0;

   int handle = 0;
   if (openStorage(BD_DRIVE_DEVICE_ID, &handle) != 0) { logError(TAG "toc: drive open failed\n"); return -1; }
   uint32_t leadoutLba = 0;
   int count = readDiscToc(handle, frameOffsets, maxTracks, &leadoutLba);
   closeStorage(handle);
   if (count < 1) { logError(TAG "toc: read failed rc=%d\n", count); return count; }

   // tracks are contiguous 1..count, so [0..count-1] are all set; shift each into a CDDB frame offset
   for (int i = 0; i < count && i < maxTracks; i++) frameOffsets[i] += CD_LEAD_IN_FRAMES;
   *leadoutFrame = leadoutLba + CD_LEAD_IN_FRAMES;

   logInfo(TAG "toc: tracks=%d leadout=%u\n", count, (unsigned)*leadoutFrame);
   return count;
}
