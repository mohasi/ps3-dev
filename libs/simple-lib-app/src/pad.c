// pad - implementation
#include "pad.h"
#include <cell/pad.h>
#include <string.h>

#define PAD_BUTTON_COUNT 16

// which digital word (1 or 2) each button lives in, and its control bit - indexed by PadButton so one
// table drives both the edge derivation and the live down-mask (no duplicated per-button mapping).
static const struct { int word; uint16_t bit; } padControlBits[PAD_BUTTON_COUNT] = {
   { 1, CELL_PAD_CTRL_UP },       { 1, CELL_PAD_CTRL_DOWN },   { 1, CELL_PAD_CTRL_LEFT },  { 1, CELL_PAD_CTRL_RIGHT },
   { 2, CELL_PAD_CTRL_CROSS },    { 2, CELL_PAD_CTRL_CIRCLE }, { 2, CELL_PAD_CTRL_SQUARE }, { 2, CELL_PAD_CTRL_TRIANGLE },
   { 2, CELL_PAD_CTRL_L1 },       { 2, CELL_PAD_CTRL_R1 },     { 2, CELL_PAD_CTRL_L2 },    { 2, CELL_PAD_CTRL_R2 },
   { 1, CELL_PAD_CTRL_START },    { 1, CELL_PAD_CTRL_SELECT }, { 1, CELL_PAD_CTRL_L3 },    { 1, CELL_PAD_CTRL_R3 },
};

static CellPadData current;
static PadButtonState buttonStates[PAD_BUTTON_COUNT];
static Stick leftStick;
static Stick rightStick;
static volatile uint16_t rawDigital1;   // latest DIGITAL1 bits from pollPad (dpad, start, select, L3/R3)
static volatile uint16_t rawDigital2;   // latest DIGITAL2 bits from pollPad (face buttons, L1/R1/L2/R2)
static uint16_t prev1 = 0;              // DIGITAL1 bits at the previous updatePadEdges, for edge detection
static uint16_t prev2 = 0;

static PadButtonState getState(int held, int wasHeld)
{
   if (held && !wasHeld) return PAD_BUTTON_STATE_PRESSED;
   if (held && wasHeld) return PAD_BUTTON_STATE_HELD;
   if (!held && wasHeld) return PAD_BUTTON_STATE_RELEASED;
   return PAD_BUTTON_STATE_UP;
}

static int buttonHeld(int button, uint16_t digital1, uint16_t digital2)
{
   uint16_t word = padControlBits[button].word == 1 ? digital1 : digital2;
   return (word & padControlBits[button].bit) != 0;
}

void initPad(void)
{
   cellPadInit(1);
   memset(&current, 0, sizeof(current));
   memset(buttonStates, 0, sizeof(buttonStates));
   memset(&leftStick, 0, sizeof(leftStick));
   memset(&rightStick, 0, sizeof(rightStick));
   rawDigital1 = 0;
   rawDigital2 = 0;
   prev1 = 0;
   prev2 = 0;
}

void pollPad(void)
{
   CellPadData nextPadData;
   if (cellPadGetData(0, &nextPadData) == CELL_OK && nextPadData.len > 0) current = nextPadData;

   rawDigital1 = current.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
   rawDigital2 = current.button[CELL_PAD_BTN_OFFSET_DIGITAL2];

   // sticks carry no edges, so publish them straight from the poll. SDK reports unsigned 0..255,
   // centered at 128; shift to signed -128..127.
   leftStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] - 128;
   leftStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] - 128;
   rightStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
   rightStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;
}

void updatePadEdges(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      buttonStates[button] = getState(buttonHeld(button, digital1, digital2), buttonHeld(button, prev1, prev2));
   prev1 = digital1;
   prev2 = digital2;
}

void updatePad(void)
{
   pollPad();
   updatePadEdges();
}

unsigned getPadDownButtons(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   unsigned mask = 0;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      if (buttonHeld(button, digital1, digital2)) mask |= 1u << button;
   return mask;
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

int isPadButtonDown(PadButton button)
{
   PadButtonState state = getPadButtonState(button);
   return state == PAD_BUTTON_STATE_PRESSED || state == PAD_BUTTON_STATE_HELD;
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
