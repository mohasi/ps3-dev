#pragma once

// pad - reads controller state

#include <stdint.h>

// button state per frame
#define BTN_UP       0  // not held
#define BTN_PRESSED  1  // just pressed this frame
#define BTN_HELD     2  // held from previous frame
#define BTN_RELEASED 3  // just released this frame

typedef struct {
	uint8_t cross;
	uint8_t circle;
	uint8_t square;
	uint8_t triangle;
	uint8_t l1, r1, l2, r2;
	uint8_t up, down, left, right;
	uint8_t start, select;
	uint8_t l3, r3;
} Buttons;

typedef struct {
	int x, y; // -128 to 127
} Stick;

typedef struct {
	Buttons btn;
	Stick   lStick;
	Stick   rStick;
} PadState;

extern PadState pad;

void padInit(void);
void padUpdate(void);


