#pragma once

// pad - hidden controller state with button/stick accessors

#include <stdint.h>

typedef enum {
   BTN_STATE_UP,
   BTN_STATE_PRESSED,
   BTN_STATE_HELD,
   BTN_STATE_RELEASED
} ButtonState;

typedef enum {
   BTN_UP,
   BTN_DOWN,
   BTN_LEFT,
   BTN_RIGHT,
   BTN_CROSS,
   BTN_CIRCLE,
   BTN_SQUARE,
   BTN_TRIANGLE,
   BTN_L1,
   BTN_R1,
   BTN_L2,
   BTN_R2,
   BTN_START,
   BTN_SELECT,
   BTN_L3,
   BTN_R3
} Button;

#define BUTTON_COUNT 16

typedef struct {
   int x;
   int y;
} Stick;

void initPad(void);
void updatePad(void);
ButtonState getButtonState(Button button);
int isButtonPressed(Button button);
int isButtonHeld(Button button);
int isButtonReleased(Button button);
Stick getLeftStick(void);
Stick getRightStick(void);
