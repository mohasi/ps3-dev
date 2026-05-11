// stats - displays fps and vram usage
#include "overlays/stats.h"
#include "font.h"
#include "gfx.h"
#include "colors.h"
#include <string.h>
#include <sys/sys_time.h>

static uint64_t lastTime = 0;
static int frameCount = 0;
static int fpsValue = 0;
static char buf[80] = "FPS: 0 | VRAM: 0";
static char lastBuf[80] = "";
static Font font;
static TextTexture tt;

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
        if (strcmp(buf, lastBuf) != 0) {
            fontRender(&tt, &font, 14, buf, COLOR_AMBER_300, AUTO, TEXT_NOWRAP);
            strncpy(lastBuf, buf, sizeof(lastBuf));
        }
    }
}

static void draw(void)
{
    if (tt.tex.w > 0)
        gfxDrawTexture(5, 5, tt.tex.w, tt.tex.h, tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}

static void term(void)
{
    fontClose(&font);
    stats.status = OVERLAY_TERMINATED;
}

Overlay stats = { show, hide, update, draw, term, OVERLAY_TERMINATED };
