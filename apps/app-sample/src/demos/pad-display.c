// pad display - debug visualization of controller state
#include "pad-display.h"
#include "pad.h"
#include "font.h"
#include "gfx.h"
#include "colors.h"

static Font *padFont;
static int padX, padY, padSize;
static uint32_t padColor;
static char buf[64];
static TextTexture ttButtons;
static TextTexture ttSticks;

static void appendInt(int *pos, int v)
{
    if (v < 0) { buf[(*pos)++] = '-'; v = -v; }
    if (v >= 100) { buf[(*pos)++] = '0' + (v / 100); v %= 100; buf[(*pos)++] = '0' + (v / 10); v %= 10; }
    else if (v >= 10) { buf[(*pos)++] = '0' + (v / 10); v %= 10; }
    buf[(*pos)++] = '0' + v;
}

void initPadDisplay(Font *f, int x, int y, int size, uint32_t color)
{
    padFont = f;
    padX = x;
    padY = y;
    padSize = size;
    padColor = color;
}

void drawPadDisplay(void)
{
    int pos = 0;
    buf[pos++] = 'B'; buf[pos++] = 'T'; buf[pos++] = 'N'; buf[pos++] = ':';
    if (pad.btn.up)       { buf[pos++] = ' '; buf[pos++] = 'U'; }
    if (pad.btn.down)     { buf[pos++] = ' '; buf[pos++] = 'D'; }
    if (pad.btn.left)     { buf[pos++] = ' '; buf[pos++] = 'L'; }
    if (pad.btn.right)    { buf[pos++] = ' '; buf[pos++] = 'R'; }
    if (pad.btn.cross)    { buf[pos++] = ' '; buf[pos++] = 'X'; }
    if (pad.btn.circle)   { buf[pos++] = ' '; buf[pos++] = 'O'; }
    if (pad.btn.square)   { buf[pos++]=' '; buf[pos++]='S'; buf[pos++]='Q'; }
    if (pad.btn.triangle) { buf[pos++]=' '; buf[pos++]='T'; buf[pos++]='R'; }
    if (pad.btn.l1)       { buf[pos++]=' '; buf[pos++]='L'; buf[pos++]='1'; }
    if (pad.btn.r1)       { buf[pos++]=' '; buf[pos++]='R'; buf[pos++]='1'; }
    if (pad.btn.l2)       { buf[pos++]=' '; buf[pos++]='L'; buf[pos++]='2'; }
    if (pad.btn.r2)       { buf[pos++]=' '; buf[pos++]='R'; buf[pos++]='2'; }
    if (pad.btn.start)    { buf[pos++]=' '; buf[pos++]='S'; buf[pos++]='T'; }
    if (pad.btn.select)   { buf[pos++]=' '; buf[pos++]='S'; buf[pos++]='E'; }
    if (pos == 4) { buf[pos++] = ' '; buf[pos++] = '-'; }
    buf[pos] = 0;
    renderFont(&ttButtons, padFont, padSize, buf, padColor, AUTO, TEXT_NOWRAP);
    if (ttButtons.tex.w > 0)
        drawGfxTexture(padX, padY, ttButtons.tex.w, ttButtons.tex.h, ttButtons.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);

    pos = 0;
    buf[pos++]='L'; buf[pos++]=':';
    appendInt(&pos, pad.lStick.x); buf[pos++]=','; appendInt(&pos, pad.lStick.y);
    buf[pos++]=' '; buf[pos++]='R'; buf[pos++]=':';
    appendInt(&pos, pad.rStick.x); buf[pos++]=','; appendInt(&pos, pad.rStick.y);
    buf[pos] = 0;
    renderFont(&ttSticks, padFont, padSize, buf, padColor, AUTO, TEXT_NOWRAP);
    if (ttSticks.tex.w > 0)
        drawGfxTexture(padX, padY + padSize + 2, ttSticks.tex.w, ttSticks.tex.h, ttSticks.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}
