// pad - implementation
#include "pad.h"
#include <cell/pad.h>
#include <string.h>

static CellPadData current;
static ButtonState buttonStates[BUTTON_COUNT];
static Stick leftStick;
static Stick rightStick;
static uint16_t prev1 = 0;  // previous frame's DIGITAL1 bits (dpad, start, select, L3/R3)
static uint16_t prev2 = 0;  // previous frame's DIGITAL2 bits (face buttons, L1/R1/L2/R2)

static ButtonState getState(int held, int wasHeld)
{
   if (held && !wasHeld) return BTN_STATE_PRESSED;
   if (held && wasHeld) return BTN_STATE_HELD;
   if (!held && wasHeld) return BTN_STATE_RELEASED;
   return BTN_STATE_UP;
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

   buttonStates[BTN_CROSS] = getState(digital2 & CELL_PAD_CTRL_CROSS, prev2 & CELL_PAD_CTRL_CROSS);
   buttonStates[BTN_CIRCLE] = getState(digital2 & CELL_PAD_CTRL_CIRCLE, prev2 & CELL_PAD_CTRL_CIRCLE);
   buttonStates[BTN_SQUARE] = getState(digital2 & CELL_PAD_CTRL_SQUARE, prev2 & CELL_PAD_CTRL_SQUARE);
   buttonStates[BTN_TRIANGLE] = getState(digital2 & CELL_PAD_CTRL_TRIANGLE, prev2 & CELL_PAD_CTRL_TRIANGLE);
   buttonStates[BTN_L1] = getState(digital2 & CELL_PAD_CTRL_L1, prev2 & CELL_PAD_CTRL_L1);
   buttonStates[BTN_R1] = getState(digital2 & CELL_PAD_CTRL_R1, prev2 & CELL_PAD_CTRL_R1);
   buttonStates[BTN_L2] = getState(digital2 & CELL_PAD_CTRL_L2, prev2 & CELL_PAD_CTRL_L2);
   buttonStates[BTN_R2] = getState(digital2 & CELL_PAD_CTRL_R2, prev2 & CELL_PAD_CTRL_R2);
   buttonStates[BTN_UP] = getState(digital1 & CELL_PAD_CTRL_UP, prev1 & CELL_PAD_CTRL_UP);
   buttonStates[BTN_DOWN] = getState(digital1 & CELL_PAD_CTRL_DOWN, prev1 & CELL_PAD_CTRL_DOWN);
   buttonStates[BTN_LEFT] = getState(digital1 & CELL_PAD_CTRL_LEFT, prev1 & CELL_PAD_CTRL_LEFT);
   buttonStates[BTN_RIGHT] = getState(digital1 & CELL_PAD_CTRL_RIGHT, prev1 & CELL_PAD_CTRL_RIGHT);
   buttonStates[BTN_START] = getState(digital1 & CELL_PAD_CTRL_START, prev1 & CELL_PAD_CTRL_START);
   buttonStates[BTN_SELECT] = getState(digital1 & CELL_PAD_CTRL_SELECT, prev1 & CELL_PAD_CTRL_SELECT);
   buttonStates[BTN_L3] = getState(digital1 & CELL_PAD_CTRL_L3, prev1 & CELL_PAD_CTRL_L3);
   buttonStates[BTN_R3] = getState(digital1 & CELL_PAD_CTRL_R3, prev1 & CELL_PAD_CTRL_R3);

   // sticks: SDK reports unsigned 0..255, centered at 128. shift to signed -128..127.
   leftStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] - 128;
   leftStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] - 128;
   rightStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
   rightStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;

   prev1 = digital1;
   prev2 = digital2;
}

ButtonState getButtonState(Button button)
{
   return buttonStates[button];
}

int isButtonPressed(Button button)
{
   return getButtonState(button) == BTN_STATE_PRESSED;
}

int isButtonHeld(Button button)
{
   return getButtonState(button) == BTN_STATE_HELD;
}

int isButtonReleased(Button button)
{
   return getButtonState(button) == BTN_STATE_RELEASED;
}

Stick getLeftStick(void)
{
   return leftStick;
}

Stick getRightStick(void)
{
   return rightStick;
}
