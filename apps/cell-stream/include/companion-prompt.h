#pragma once

// companion-prompt - the screen shown while no stream is arriving: it tells the user the Windows
// companion server is needed and shows a QR code to its release page. call drawCompanionPrompt()
// in place of the plain "waiting" text whenever the stream is not live.

#include "font.h"

void initCompanionPrompt(Font *font);
void drawCompanionPrompt(void);
void freeCompanionPrompt(void);
