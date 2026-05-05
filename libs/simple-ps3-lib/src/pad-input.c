// pad input - implementation
#include "pad-input.h"
#include <cell/pad.h>
#include <string.h>

PadState pad;

static CellPadData padCurrent;
static uint16_t padPrevD1 = 0;
static uint16_t padPrevD2 = 0;
static char padBuf[64];

static uint8_t btnState(int held, int wasHeld)
{
    if (held && !wasHeld) return BTN_PRESSED;
    if (held && wasHeld)  return BTN_HELD;
    if (!held && wasHeld) return BTN_RELEASED;
    return BTN_UP;
}

void padInit(void)
{
    cellPadInit(1);
    memset(&pad, 0, sizeof(pad));
}

void padUpdate(void)
{
    CellPadData tmp;
    if (cellPadGetData(0, &tmp) == CELL_OK && tmp.len > 0) {
        padCurrent = tmp;
    }

    uint16_t d1 = padCurrent.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
    uint16_t d2 = padCurrent.button[CELL_PAD_BTN_OFFSET_DIGITAL2];

    pad.btn.cross    = btnState(d2 & CELL_PAD_CTRL_CROSS,    padPrevD2 & CELL_PAD_CTRL_CROSS);
    pad.btn.circle   = btnState(d2 & CELL_PAD_CTRL_CIRCLE,   padPrevD2 & CELL_PAD_CTRL_CIRCLE);
    pad.btn.square   = btnState(d2 & CELL_PAD_CTRL_SQUARE,   padPrevD2 & CELL_PAD_CTRL_SQUARE);
    pad.btn.triangle = btnState(d2 & CELL_PAD_CTRL_TRIANGLE, padPrevD2 & CELL_PAD_CTRL_TRIANGLE);
    pad.btn.l1       = btnState(d2 & CELL_PAD_CTRL_L1,       padPrevD2 & CELL_PAD_CTRL_L1);
    pad.btn.r1       = btnState(d2 & CELL_PAD_CTRL_R1,       padPrevD2 & CELL_PAD_CTRL_R1);
    pad.btn.l2       = btnState(d2 & CELL_PAD_CTRL_L2,       padPrevD2 & CELL_PAD_CTRL_L2);
    pad.btn.r2       = btnState(d2 & CELL_PAD_CTRL_R2,       padPrevD2 & CELL_PAD_CTRL_R2);
    pad.btn.up       = btnState(d1 & CELL_PAD_CTRL_UP,       padPrevD1 & CELL_PAD_CTRL_UP);
    pad.btn.down     = btnState(d1 & CELL_PAD_CTRL_DOWN,     padPrevD1 & CELL_PAD_CTRL_DOWN);
    pad.btn.left     = btnState(d1 & CELL_PAD_CTRL_LEFT,     padPrevD1 & CELL_PAD_CTRL_LEFT);
    pad.btn.right    = btnState(d1 & CELL_PAD_CTRL_RIGHT,    padPrevD1 & CELL_PAD_CTRL_RIGHT);
    pad.btn.start    = btnState(d1 & CELL_PAD_CTRL_START,    padPrevD1 & CELL_PAD_CTRL_START);
    pad.btn.select   = btnState(d1 & CELL_PAD_CTRL_SELECT,   padPrevD1 & CELL_PAD_CTRL_SELECT);

    pad.lStick.x = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] - 128;
    pad.lStick.y = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] - 128;
    pad.rStick.x = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
    pad.rStick.y = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;

    padPrevD1 = d1;
    padPrevD2 = d2;
}

static void padAppendInt(int *pos, int v)
{
    if (v < 0) { padBuf[(*pos)++] = '-'; v = -v; }
    if (v >= 100) { padBuf[(*pos)++] = '0' + (v / 100); v %= 100; padBuf[(*pos)++] = '0' + (v / 10); v %= 10; }
    else if (v >= 10) { padBuf[(*pos)++] = '0' + (v / 10); v %= 10; }
    padBuf[(*pos)++] = '0' + v;
}

void padDraw(int x, int y, Font *f, int size, uint32_t color)
{
    if (padCurrent.len == 0) {
        fontDraw(x, y, TEXT_AUTOSIZE, TEXT_AUTOSIZE, "pad: none", f, size, color, TEXT_NOWRAP);
        return;
    }

    int pos = 0;
    padBuf[pos++] = 'B'; padBuf[pos++] = 'T'; padBuf[pos++] = 'N'; padBuf[pos++] = ':';
    if (pad.btn.up)       { padBuf[pos++] = ' '; padBuf[pos++] = 'U'; }
    if (pad.btn.down)     { padBuf[pos++] = ' '; padBuf[pos++] = 'D'; }
    if (pad.btn.left)     { padBuf[pos++] = ' '; padBuf[pos++] = 'L'; }
    if (pad.btn.right)    { padBuf[pos++] = ' '; padBuf[pos++] = 'R'; }
    if (pad.btn.cross)    { padBuf[pos++] = ' '; padBuf[pos++] = 'X'; }
    if (pad.btn.circle)   { padBuf[pos++] = ' '; padBuf[pos++] = 'O'; }
    if (pad.btn.square)   { padBuf[pos++]=' '; padBuf[pos++]='S'; padBuf[pos++]='Q'; }
    if (pad.btn.triangle) { padBuf[pos++]=' '; padBuf[pos++]='T'; padBuf[pos++]='R'; }
    if (pad.btn.l1)       { padBuf[pos++]=' '; padBuf[pos++]='L'; padBuf[pos++]='1'; }
    if (pad.btn.r1)       { padBuf[pos++]=' '; padBuf[pos++]='R'; padBuf[pos++]='1'; }
    if (pad.btn.l2)       { padBuf[pos++]=' '; padBuf[pos++]='L'; padBuf[pos++]='2'; }
    if (pad.btn.r2)       { padBuf[pos++]=' '; padBuf[pos++]='R'; padBuf[pos++]='2'; }
    if (pad.btn.start)    { padBuf[pos++]=' '; padBuf[pos++]='S'; padBuf[pos++]='T'; }
    if (pad.btn.select)   { padBuf[pos++]=' '; padBuf[pos++]='S'; padBuf[pos++]='E'; }
    if (pos == 4) { padBuf[pos++] = ' '; padBuf[pos++] = '-'; }
    padBuf[pos] = 0;
    fontDraw(x, y, TEXT_AUTOSIZE, TEXT_AUTOSIZE, padBuf, f, size, color, TEXT_NOWRAP);

    pos = 0;
    padBuf[pos++]='L'; padBuf[pos++]=':';
    padAppendInt(&pos, pad.lStick.x); padBuf[pos++]=','; padAppendInt(&pos, pad.lStick.y);
    padBuf[pos++]=' '; padBuf[pos++]='R'; padBuf[pos++]=':';
    padAppendInt(&pos, pad.rStick.x); padBuf[pos++]=','; padAppendInt(&pos, pad.rStick.y);
    padBuf[pos] = 0;
    fontDraw(x, y + size + 2, TEXT_AUTOSIZE, TEXT_AUTOSIZE, padBuf, f, size, color, TEXT_NOWRAP);
}
