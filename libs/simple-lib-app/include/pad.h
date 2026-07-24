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

// One reader owns the controller hardware. In a single-threaded app just call updatePad() each frame,
// as before. To sample the pad on a dedicated thread (e.g. to forward input at a steady rate, off the
// render loop), have that thread call pollPad() and the UI thread call updatePadEdges() - so the two
// never both touch the hardware.
void pollPad(void);          // read the controller hardware into shared state (the sole hardware reader)
void updatePadEdges(void);   // recompute button press/hold/release edges from the last pollPad
void updatePad(void);        // pollPad + updatePadEdges together, for the common single-thread case

PadButtonState getPadButtonState(PadButton button);
int isPadButtonPressed(PadButton button);   // the frame it went down
int isPadButtonHeld(PadButton button);      // frames after that, NOT the frame it went down
int isPadButtonDown(PadButton button);      // down right now, however long it has been: pressed or held
int isPadButtonReleased(PadButton button);
unsigned getPadDownButtons(void);           // buttons down right now as a mask (1u << PadButton), for forwarding
Stick getPadLeftStick(void);
Stick getPadRightStick(void);
