// free-space-widget - hdd free space label refreshed periodically
#include "widgets/free-space-widget.h"
#include "ui/label.h"
#include "timer.h"
#include "vfs.h"
#include "string-utilities.h"

static Label label;
static Timer timer;
static char freeSpacePath[MAX_PATH_LEN] = "/dev_hdd0/";   // volume to report; tracks the current dir

static void refresh(void *ctx)
{
   (void)ctx;
   uint64_t freeBytes = 0;
   if (getFreeSpace(freeSpacePath, &freeBytes, NULL) != 0) {
      setLabelText(&label, "Free Space : " EM_DASH);   // no single volume here (e.g. the device-list root)
      return;
   }

   uint64_t freeGB = freeBytes / (1024 * 1024 * 1024);

   char buf[32];
   int p = 0;
   appendStr(buf, sizeof buf, &p, "Free Space : ");
   p += intToDec((int)freeGB, buf + p);
   buf[p++] = ' ';
   buf[p++] = 'G';
   buf[p++] = 'B';
   buf[p] = '\0';

   setLabelText(&label, buf);
}

// points the widget at the volume owning path (the directory the user is in) and
// refreshes now. cheap no-op when the volume hasn't changed since last call.
void setFreeSpacePath(const char *path)
{
   if (!path || strEq(freeSpacePath, path)) return;
   strCopy(freeSpacePath, sizeof freeSpacePath, path);
   refresh(NULL);
}

void initFreeSpaceWidget(Font *font, int x, int y, int size, uint32_t color, int width)
{
   initLabel(&label, font, x, y, width, AUTO, size, color, TEXT_NOWRAP, NULL);
   initTimer(&timer, 10000000, refresh, NULL);
   refresh(NULL);
}

void updateFreeSpaceWidget(void)
{
   updateTimer(&timer);
}

void drawFreeSpaceWidget(void)
{
   drawLabel(&label);
}

void termFreeSpaceWidget(void)
{
   freeLabel(&label);
}
