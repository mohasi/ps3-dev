// free-space-widget - hdd free space label refreshed periodically
#include "widgets/free-space-widget.h"
#include "ui/label.h"
#include "timer.h"
#include "file.h"
#include "string-utilities.h"
#include <cell/cell_fs.h>

static Label label;
static Timer timer;

static void refresh(void *ctx)
{
    (void)ctx;
    uint32_t blockSize;
    uint64_t freeBlocks;
    if (cellFsGetFreeSize("/dev_hdd0/", &blockSize, &freeBlocks) != CELL_FS_SUCCEEDED) return;

    uint64_t freeGB = (uint64_t)blockSize * freeBlocks / (1024 * 1024 * 1024);

    char buf[8];
    int p = intToDec((int)freeGB, buf);
    buf[p++] = ' ';
    buf[p++] = 'G';
    buf[p++] = 'B';
    buf[p] = '\0';

    setLabelText(&label, buf);
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
