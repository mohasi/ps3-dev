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

void termPadDisplay(void)
{
    freeTextTexture(&ttButtons);
    freeTextTexture(&ttSticks);
}

void drawPadDisplay(void)
{
    int pos = 0;
    buf[pos++] = 'B'; buf[pos++] = 'T'; buf[pos++] = 'N'; buf[pos++] = ':';
    if (getPadButtonState(PAD_BTN_UP))       { buf[pos++] = ' '; buf[pos++] = 'U'; }
    if (getPadButtonState(PAD_BTN_DOWN))     { buf[pos++] = ' '; buf[pos++] = 'D'; }
    if (getPadButtonState(PAD_BTN_LEFT))     { buf[pos++] = ' '; buf[pos++] = 'L'; }
    if (getPadButtonState(PAD_BTN_RIGHT))    { buf[pos++] = ' '; buf[pos++] = 'R'; }
    if (getPadButtonState(PAD_BTN_CROSS))    { buf[pos++] = ' '; buf[pos++] = 'X'; }
    if (getPadButtonState(PAD_BTN_CIRCLE))   { buf[pos++] = ' '; buf[pos++] = 'O'; }
    if (getPadButtonState(PAD_BTN_SQUARE))   { buf[pos++]=' '; buf[pos++]='S'; buf[pos++]='Q'; }
    if (getPadButtonState(PAD_BTN_TRIANGLE)) { buf[pos++]=' '; buf[pos++]='T'; buf[pos++]='R'; }
    if (getPadButtonState(PAD_BTN_L1))       { buf[pos++]=' '; buf[pos++]='L'; buf[pos++]='1'; }
    if (getPadButtonState(PAD_BTN_R1))       { buf[pos++]=' '; buf[pos++]='R'; buf[pos++]='1'; }
    if (getPadButtonState(PAD_BTN_L2))       { buf[pos++]=' '; buf[pos++]='L'; buf[pos++]='2'; }
    if (getPadButtonState(PAD_BTN_R2))       { buf[pos++]=' '; buf[pos++]='R'; buf[pos++]='2'; }
    if (getPadButtonState(PAD_BTN_START))    { buf[pos++]=' '; buf[pos++]='S'; buf[pos++]='T'; }
    if (getPadButtonState(PAD_BTN_SELECT))   { buf[pos++]=' '; buf[pos++]='S'; buf[pos++]='E'; }
    if (getPadButtonState(PAD_BTN_L3))       { buf[pos++]=' '; buf[pos++]='L'; buf[pos++]='3'; }
    if (getPadButtonState(PAD_BTN_R3))       { buf[pos++]=' '; buf[pos++]='R'; buf[pos++]='3'; }
    if (pos == 4) { buf[pos++] = ' '; buf[pos++] = '-'; }
    buf[pos] = 0;
    renderFont(&ttButtons, padFont, padSize, buf, padColor, AUTO, TEXT_NOWRAP);
    if (ttButtons.tex.w > 0)
        drawGfxTexture(padX, padY, ttButtons.tex.w, ttButtons.tex.h, ttButtons.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);

    pos = 0;
    buf[pos++]='L'; buf[pos++]=':';
    appendInt(&pos, getPadLeftStick().x); buf[pos++]=','; appendInt(&pos, getPadLeftStick().y);
    buf[pos++]=' '; buf[pos++]='R'; buf[pos++]=':';
    appendInt(&pos, getPadRightStick().x); buf[pos++]=','; appendInt(&pos, getPadRightStick().y);
    buf[pos] = 0;
    renderFont(&ttSticks, padFont, padSize, buf, padColor, AUTO, TEXT_NOWRAP);
    if (ttSticks.tex.w > 0)
        drawGfxTexture(padX, padY + padSize + 2, ttSticks.tex.w, ttSticks.tex.h, ttSticks.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}
