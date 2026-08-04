#pragma once

// toast-widget - a short message that appears near the bottom of the screen and
// disappears on its own. for outcomes the user should see but need not confirm.

#include "font.h"

void initToastWidget(Font *font);
void showToast(const char *message);
void updateToastWidget(void);
void rethemeToastWidget(void);
void drawToastWidget(void);
void termToastWidget(void);
