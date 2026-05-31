// pad - implementation
#include "pad.h"
#include <cell/pad.h>
#include <string.h>

PadState pad;

static CellPadData current;
static uint16_t prev1 = 0;  // previous frame's DIGITAL1 bits (dpad, start, select, L3/R3)
static uint16_t prev2 = 0;  // previous frame's DIGITAL2 bits (face buttons, L1/R1/L2/R2)

static inline uint8_t getState(int held, int wasHeld)
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
        current = tmp;
    }

    uint16_t d1 = current.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
    uint16_t d2 = current.button[CELL_PAD_BTN_OFFSET_DIGITAL2];

    pad.btn.cross    = getState(d2 & CELL_PAD_CTRL_CROSS,    prev2 & CELL_PAD_CTRL_CROSS);
    pad.btn.circle   = getState(d2 & CELL_PAD_CTRL_CIRCLE,   prev2 & CELL_PAD_CTRL_CIRCLE);
    pad.btn.square   = getState(d2 & CELL_PAD_CTRL_SQUARE,   prev2 & CELL_PAD_CTRL_SQUARE);
    pad.btn.triangle = getState(d2 & CELL_PAD_CTRL_TRIANGLE, prev2 & CELL_PAD_CTRL_TRIANGLE);
    pad.btn.l1       = getState(d2 & CELL_PAD_CTRL_L1,       prev2 & CELL_PAD_CTRL_L1);
    pad.btn.r1       = getState(d2 & CELL_PAD_CTRL_R1,       prev2 & CELL_PAD_CTRL_R1);
    pad.btn.l2       = getState(d2 & CELL_PAD_CTRL_L2,       prev2 & CELL_PAD_CTRL_L2);
    pad.btn.r2       = getState(d2 & CELL_PAD_CTRL_R2,       prev2 & CELL_PAD_CTRL_R2);
    pad.btn.up       = getState(d1 & CELL_PAD_CTRL_UP,       prev1 & CELL_PAD_CTRL_UP);
    pad.btn.down     = getState(d1 & CELL_PAD_CTRL_DOWN,     prev1 & CELL_PAD_CTRL_DOWN);
    pad.btn.left     = getState(d1 & CELL_PAD_CTRL_LEFT,     prev1 & CELL_PAD_CTRL_LEFT);
    pad.btn.right    = getState(d1 & CELL_PAD_CTRL_RIGHT,    prev1 & CELL_PAD_CTRL_RIGHT);
    pad.btn.start    = getState(d1 & CELL_PAD_CTRL_START,    prev1 & CELL_PAD_CTRL_START);
    pad.btn.select   = getState(d1 & CELL_PAD_CTRL_SELECT,   prev1 & CELL_PAD_CTRL_SELECT);
    pad.btn.l3       = getState(d1 & CELL_PAD_CTRL_L3,       prev1 & CELL_PAD_CTRL_L3);
    pad.btn.r3       = getState(d1 & CELL_PAD_CTRL_R3,       prev1 & CELL_PAD_CTRL_R3);

    // sticks: SDK reports unsigned 0..255, centered at 128. shift to signed -128..127.
    pad.lStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  - 128;
    pad.lStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  - 128;
    pad.rStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
    pad.rStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;

    prev1 = d1;
    prev2 = d2;
}
