#pragma once

// pad - hidden controller state with button/stick accessors

#include <stdint.h>

typedef enum {
   PAD_BUTTON_STATE_UP,
   PAD_BUTTON_STATE_PRESSED,
   PAD_BUTTON_STATE_HELD,
   PAD_BUTTON_STATE_RELEASED
} PadButtonState;

typedef enum {
   PAD_BTN_UP,
   PAD_BTN_DOWN,
   PAD_BTN_LEFT,
   PAD_BTN_RIGHT,
   PAD_BTN_CROSS,
   PAD_BTN_CIRCLE,
   PAD_BTN_SQUARE,
   PAD_BTN_TRIANGLE,
   PAD_BTN_L1,
   PAD_BTN_R1,
   PAD_BTN_L2,
   PAD_BTN_R2,
   PAD_BTN_START,
   PAD_BTN_SELECT,
   PAD_BTN_L3,
   PAD_BTN_R3
} PadButton;

typedef struct {
   int x;
   int y;
} Stick;

void initPad(void);
void updatePad(void);
PadButtonState getPadButtonState(PadButton button);
int isPadButtonPressed(PadButton button);
int isPadButtonHeld(PadButton button);
int isPadButtonReleased(PadButton button);
Stick getPadLeftStick(void);
Stick getPadRightStick(void);
