#pragma once

// shortcut-hint - a bottom bar listing the current shortcuts. shown the whole time while waiting
// for a stream, then kept for a few more seconds once one starts, then gone. drawShortcutHint()
// renders the bar whenever the caller asks; the caller shows it always while waiting, and only
// while isShortcutHintActive() is true once streaming.

#include "font.h"

void initShortcutHint(Font *font);
void showShortcutHint(void);       // arm the few-second window - call when a stream goes live
void updateShortcutHint(void);     // expire that window
int  isShortcutHintActive(void);   // still within the post-start window
void drawShortcutHint(void);       // render the bar (the caller decides when to call it)
void freeShortcutHint(void);

// the stats panel's own "[SELECT][stats] Hide" footer, centred within [0, centerWidth]
int  getStatsHideHintHeight(void);
void drawStatsHideHint(int centerWidth, int y);
