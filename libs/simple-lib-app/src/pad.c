// pad - implementation
#include "pad.h"
#include <cell/pad.h>
#include <string.h>

#define PAD_BUTTON_COUNT 16

static CellPadData current;
static PadButtonState buttonStates[PAD_BUTTON_COUNT];
static Stick leftStick;
static Stick rightStick;
static uint16_t prev1 = 0;  // previous frame's DIGITAL1 bits (dpad, start, select, L3/R3)
static uint16_t prev2 = 0;  // previous frame's DIGITAL2 bits (face buttons, L1/R1/L2/R2)

static PadButtonState getState(int held, int wasHeld)
{
   if (held && !wasHeld) return PAD_BUTTON_STATE_PRESSED;
   if (held && wasHeld) return PAD_BUTTON_STATE_HELD;
   if (!held && wasHeld) return PAD_BUTTON_STATE_RELEASED;
   return PAD_BUTTON_STATE_UP;
}

void initPad(void)
{
   cellPadInit(1);
   memset(&current, 0, sizeof(current));
   memset(buttonStates, 0, sizeof(buttonStates));
   memset(&leftStick, 0, sizeof(leftStick));
   memset(&rightStick, 0, sizeof(rightStick));
   prev1 = 0;
   prev2 = 0;
}

void updatePad(void)
{
   CellPadData nextPadData;
   if (cellPadGetData(0, &nextPadData) == CELL_OK && nextPadData.len > 0) current = nextPadData;

   uint16_t digital1 = current.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
   uint16_t digital2 = current.button[CELL_PAD_BTN_OFFSET_DIGITAL2];

   buttonStates[PAD_BTN_CROSS] = getState(digital2 & CELL_PAD_CTRL_CROSS, prev2 & CELL_PAD_CTRL_CROSS);
   buttonStates[PAD_BTN_CIRCLE] = getState(digital2 & CELL_PAD_CTRL_CIRCLE, prev2 & CELL_PAD_CTRL_CIRCLE);
   buttonStates[PAD_BTN_SQUARE] = getState(digital2 & CELL_PAD_CTRL_SQUARE, prev2 & CELL_PAD_CTRL_SQUARE);
   buttonStates[PAD_BTN_TRIANGLE] = getState(digital2 & CELL_PAD_CTRL_TRIANGLE, prev2 & CELL_PAD_CTRL_TRIANGLE);
   buttonStates[PAD_BTN_L1] = getState(digital2 & CELL_PAD_CTRL_L1, prev2 & CELL_PAD_CTRL_L1);
   buttonStates[PAD_BTN_R1] = getState(digital2 & CELL_PAD_CTRL_R1, prev2 & CELL_PAD_CTRL_R1);
   buttonStates[PAD_BTN_L2] = getState(digital2 & CELL_PAD_CTRL_L2, prev2 & CELL_PAD_CTRL_L2);
   buttonStates[PAD_BTN_R2] = getState(digital2 & CELL_PAD_CTRL_R2, prev2 & CELL_PAD_CTRL_R2);
   buttonStates[PAD_BTN_UP] = getState(digital1 & CELL_PAD_CTRL_UP, prev1 & CELL_PAD_CTRL_UP);
   buttonStates[PAD_BTN_DOWN] = getState(digital1 & CELL_PAD_CTRL_DOWN, prev1 & CELL_PAD_CTRL_DOWN);
   buttonStates[PAD_BTN_LEFT] = getState(digital1 & CELL_PAD_CTRL_LEFT, prev1 & CELL_PAD_CTRL_LEFT);
   buttonStates[PAD_BTN_RIGHT] = getState(digital1 & CELL_PAD_CTRL_RIGHT, prev1 & CELL_PAD_CTRL_RIGHT);
   buttonStates[PAD_BTN_START] = getState(digital1 & CELL_PAD_CTRL_START, prev1 & CELL_PAD_CTRL_START);
   buttonStates[PAD_BTN_SELECT] = getState(digital1 & CELL_PAD_CTRL_SELECT, prev1 & CELL_PAD_CTRL_SELECT);
   buttonStates[PAD_BTN_L3] = getState(digital1 & CELL_PAD_CTRL_L3, prev1 & CELL_PAD_CTRL_L3);
   buttonStates[PAD_BTN_R3] = getState(digital1 & CELL_PAD_CTRL_R3, prev1 & CELL_PAD_CTRL_R3);

   // sticks: SDK reports unsigned 0..255, centered at 128. shift to signed -128..127.
   leftStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] - 128;
   leftStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] - 128;
   rightStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
   rightStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;

   prev1 = digital1;
   prev2 = digital2;
}

PadButtonState getPadButtonState(PadButton button)
{
   return buttonStates[button];
}

int isPadButtonPressed(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_PRESSED;
}

int isPadButtonHeld(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_HELD;
}

int isPadButtonReleased(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_RELEASED;
}

Stick getPadLeftStick(void)
{
   return leftStick;
}

Stick getPadRightStick(void)
{
   return rightStick;
}
