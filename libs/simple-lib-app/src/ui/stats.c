// stats - fps and vram usage display
#include "ui/stats.h"
#include "font.h"
#include "gfx.h"
#include "colors.h"
#include "string-utilities.h"
#include <string.h>
#include <sys/sys_time.h>

static Font font;
static int posX, posY, fontSize;
static uint32_t color;
static TextTexture tt;
static int fps;
static int frameCount;
static uint64_t lastTime;
static char buf[48];
static char lastBuf[48];

static void appendInt(char *dst, int *pos, size_t v)
{
    char tmp[16];
    int len = 0;
    if (v == 0) { dst[(*pos)++] = '0'; return; }
    while (v > 0) { tmp[len++] = '0' + (v % 10); v /= 10; }
    for (int i = len - 1; i >= 0; i--) {
        dst[(*pos)++] = tmp[i];
        if (i > 0 && i % 3 == 0) dst[(*pos)++] = ',';
    }
}

static void buildBuf(void)
{
    int pos = 0;
    buf[pos++]='F'; buf[pos++]='P'; buf[pos++]='S'; buf[pos++]=':'; buf[pos++]=' ';
    appendInt(buf, &pos, fps);
    buf[pos++]=' '; buf[pos++]='|'; buf[pos++]=' ';
    buf[pos++]='V'; buf[pos++]='R'; buf[pos++]='A'; buf[pos++]='M'; buf[pos++]=':'; buf[pos++]=' ';
    appendInt(buf, &pos, getUsedGfxVram());
    buf[pos] = 0;
}

void initStats(int x, int y, int size, uint32_t clr)
{
    font = openSystemFont(FONT_POP);
    posX = x;
    posY = y;
    fontSize = size;
    color = clr;
    fps = 0;
    frameCount = 0;
    lastTime = 0;
    buf[0] = 0;
    lastBuf[0] = 0;
}

void drawStats(void)
{
    uint64_t now = sys_time_get_system_time();
    frameCount++;
    if (lastTime == 0) { lastTime = now; return; }
    uint64_t elapsed = now - lastTime;
    if (elapsed >= 1000000) {
        fps = frameCount;
        frameCount = 0;
        lastTime = now;
        buildBuf();
        if (strcmp(buf, lastBuf) != 0) {
            renderFont(&tt, &font, fontSize, buf, color, AUTO, TEXT_NOWRAP);
            strCopy(lastBuf, sizeof lastBuf, buf);
        }
    }
    if (tt.tex.w > 0)
        drawGfxTexture(posX, posY, tt.tex.w, tt.tex.h, tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}
