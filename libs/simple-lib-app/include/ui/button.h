#pragma once

// button - icon + label button with enabled/disabled rendering

#include "ui/image.h"
#include "ui/label.h"

#define BUTTON_ICON_LABEL_GAP 14
#define BUTTON_DISABLED_ALPHA 0x66

typedef enum {
	BUTTON_ENABLED = 0,
	BUTTON_DISABLED
} ButtonState;

typedef struct {
	Image icon;
	Label label;
	ButtonState state;
} Button;

void initButton(Button *button, Image icon, Label label, ButtonState state);
void setButtonState(Button *button, ButtonState state);
void moveButton(Button *button, int x, int y);
int  getButtonWidth(Button *button);
void drawButton(Button *button);
