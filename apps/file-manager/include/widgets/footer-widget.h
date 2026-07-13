#pragma once

// footer-widget - bottom action row of icon + label buttons

#include "font.h"
#include "pad.h"
#include "ui/console-glyphs.h"

typedef void (*FooterButtonHandler)(void);

void initFooterWidget(Font *font);
void addFooterButton(PadButton padButton, ConsoleGlyph glyph, const char *text, FooterButtonHandler onPress);
void setFooterButtonEnabled(PadButton padButton, int enabled);
void setFooterButtonVisible(PadButton padButton, int visible);
void setFooterButtonText(PadButton padButton, const char *text);
void updateFooterWidget(void);
void drawFooterWidget(void);
void termFooterWidget(void);
