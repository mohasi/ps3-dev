// stats - displays fps and vram usage
#include "overlays/stats.h"
#include "font.h"
#include "gfx.h"
#include "colors.h"
#include <sys/sys_time.h>

static uint64_t lastTime = 0;
static int frameCount = 0;
static int fpsValue = 0;
static char buf[80] = "FPS: 0 | VRAM: 0 (perm) 0 (per-frame)";
static Font font;

static void appendInt(char *dst, int *pos, size_t v)
{
    char tmp[16];
    int len = 0;
    if (v == 0) { dst[(*pos)++] = '0'; return; }
    while (v > 0) { tmp[len++] = '0' + (v % 10); v /= 10; }
    // insert with commas every 3 digits
    for (int i = len - 1; i >= 0; i--) {
        dst[(*pos)++] = tmp[i];
        if (i > 0 && i % 3 == 0) dst[(*pos)++] = ',';
    }
}

static void buildBuf(void)
{
    int pos = 0;
    buf[pos++]='F'; buf[pos++]='P'; buf[pos++]='S'; buf[pos++]=':'; buf[pos++]=' ';
    appendInt(buf, &pos, fpsValue);
    buf[pos++]=' '; buf[pos++]='|'; buf[pos++]=' ';
    buf[pos++]='V'; buf[pos++]='R'; buf[pos++]='A'; buf[pos++]='M'; buf[pos++]=':'; buf[pos++]=' ';
    appendInt(buf, &pos, gfxVramUsed());
    buf[pos++]=' '; buf[pos++]='('; buf[pos++]='p'; buf[pos++]='e'; buf[pos++]='r'; buf[pos++]='m'; buf[pos++]=')';
    buf[pos++]=' ';
    appendInt(buf, &pos, gfxVramUsedTemp());
    buf[pos++]=' '; buf[pos++]='('; buf[pos++]='p'; buf[pos++]='e'; buf[pos++]='r';
    buf[pos++]='-'; buf[pos++]='f'; buf[pos++]='r'; buf[pos++]='a'; buf[pos++]='m'; buf[pos++]='e'; buf[pos++]=')';
    buf[pos] = 0;
}

static void init(void)
{
    font = fontOpenSystem(FONT_POP);
    lastTime = 0;
    frameCount = 0;
    fpsValue = 0;
}

static void show(void)
{
    if (stats.status == OVERLAY_TERMINATED) init();
    buildBuf();
    stats.status = OVERLAY_VISIBLE;
}

static void hide(void)
{
    stats.status = OVERLAY_HIDDEN;
}

static void update(void)
{
    uint64_t now = sys_time_get_system_time();
    frameCount++;
    if (lastTime == 0) { lastTime = now; return; }
    uint64_t elapsed = now - lastTime;
    if (elapsed >= 1000000) {
        fpsValue = frameCount;
        frameCount = 0;
        lastTime = now;
        buildBuf();
    }
}

static void draw(void)
{
    fontDraw(5, -10, AUTO, AUTO, buf, &font, 14, COLOR_AMBER_300, TEXT_NOWRAP);
}

static void term(void)
{
    fontClose(&font);
    stats.status = OVERLAY_TERMINATED;
}

Overlay stats = { show, hide, update, draw, term, OVERLAY_TERMINATED };
