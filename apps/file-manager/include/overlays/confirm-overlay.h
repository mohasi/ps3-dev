#pragma once

#include "overlay.h"
#include <stdbool.h>

typedef void (*ConfirmCallback)(bool confirmed);

void askConfirm(const char *title, const char *message, ConfirmCallback onResult);

extern Overlay confirmOverlay;
