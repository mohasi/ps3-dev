// pad - implementation
#include "pad.h"
#include <cell/pad.h>
#include <string.h>

PadState pad;

static CellPadData padCurrent;
static uint16_t padPrevD1 = 0;
static uint16_t padPrevD2 = 0;

static uint8_t btnState(int held, int wasHeld)
{
    if (held && !wasHeld) return BTN_PRESSED;
    if (held && wasHeld)  return BTN_HELD;
    if (!held && wasHeld) return BTN_RELEASED;
    return BTN_UP;
}

void initPad(void)
{
    cellPadInit(1);
    memset(&pad, 0, sizeof(pad));
}

void updatePad(void)
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
    pad.btn.l3       = btnState(d1 & CELL_PAD_CTRL_L3,       padPrevD1 & CELL_PAD_CTRL_L3);
    pad.btn.r3       = btnState(d1 & CELL_PAD_CTRL_R3,       padPrevD1 & CELL_PAD_CTRL_R3);

    pad.lStick.x = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] - 128;
    pad.lStick.y = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] - 128;
    pad.rStick.x = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
    pad.rStick.y = padCurrent.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;

    padPrevD1 = d1;
    padPrevD2 = d2;
}
