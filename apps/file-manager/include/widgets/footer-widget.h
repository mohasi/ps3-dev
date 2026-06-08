#pragma once

// footer-widget - bottom action row of icon + label buttons

#include "font.h"
#include "gfx.h"
#include "pad.h"

#define FOOTER_MAX_BUTTONS 8

typedef void (*FooterButtonHandler)(void);

void initFooterWidget(Font *font, GfxTexture spritesheet);
void addFooterButton(PadButton padButton, SpriteRegion iconRegion, const char *text, FooterButtonHandler onPress);
void setFooterButtonEnabled(PadButton padButton, int enabled);
void updateFooterWidget(void);
void drawFooterWidget(void);
