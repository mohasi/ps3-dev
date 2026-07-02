// footer-widget - bottom action row of icon + label buttons
#include "widgets/footer-widget.h"
#include "colors.h"
#include "ui/button.h"
#include "ui/image.h"
#include "ui/label.h"

#define FOOTER_MAX_BUTTONS      8
#define FOOTER_X               51
#define FOOTER_Y             1015
#define FOOTER_TEXT_SIZE       20
#define FOOTER_GROUP_GAP       80

typedef struct {
   PadButton           padButton;
   Button              button;
   FooterButtonHandler onPress;
   int                 visible;
} FooterEntry;

static FooterEntry entries[FOOTER_MAX_BUTTONS];
static int         entryCount;
static Font       *footerFont;
static GfxTexture  footerSpritesheet;

static FooterEntry *findFooterEntry(PadButton padButton)
{
   for (int i = 0; i < entryCount; i++)
      if (entries[i].padButton == padButton) return &entries[i];
   return NULL;
}

void initFooterWidget(Font *font, GfxTexture spritesheet)
{
   footerFont        = font;
   footerSpritesheet = spritesheet;
   entryCount        = 0;
}

void addFooterButton(PadButton padButton, SpriteRegion iconRegion, const char *text, FooterButtonHandler onPress)
{
   FooterEntry *entry = findFooterEntry(padButton);
   if (entry) {
      freeButton(&entry->button);  // release the previous label before replacing it
   } else {
      if (entryCount >= FOOTER_MAX_BUTTONS) return;
      entry = &entries[entryCount++];
   }

   Image icon;
   Label label;
   initImage(&icon, footerSpritesheet, 0, 0, AUTO, AUTO, iconRegion, GFX_FILTER_LINEAR);
   initLabel(&label, footerFont, 0, 0, AUTO, AUTO, FOOTER_TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, text);

   entry->padButton = padButton;
   entry->onPress   = onPress;
   entry->visible   = 1;
   initButton(&entry->button, icon, label, BUTTON_ENABLED);
}

void setFooterButtonEnabled(PadButton padButton, int enabled)
{
   FooterEntry *entry = findFooterEntry(padButton);
   if (!entry) return;
   setButtonState(&entry->button, enabled ? BUTTON_ENABLED : BUTTON_DISABLED);
}

void setFooterButtonVisible(PadButton padButton, int visible)
{
   FooterEntry *entry = findFooterEntry(padButton);
   if (!entry) return;
   entry->visible = visible;
}

void setFooterButtonText(PadButton padButton, const char *text)
{
   FooterEntry *entry = findFooterEntry(padButton);
   if (!entry) return;
   setLabelText(&entry->button.label, text);
}

void updateFooterWidget(void)
{
   for (int i = 0; i < entryCount; i++) {
      if (!entries[i].visible) continue;
      if (!entries[i].onPress) continue;
      if (entries[i].button.state == BUTTON_DISABLED) continue;
      if (!isPadButtonPressed(entries[i].padButton)) continue;

      entries[i].onPress();
   }
}

void termFooterWidget(void)
{
   for (int i = 0; i < entryCount; i++)
      freeButton(&entries[i].button);
   entryCount = 0;
}

void drawFooterWidget(void)
{
   int x = FOOTER_X;
   for (int i = 0; i < entryCount; i++) {
      if (!entries[i].visible) continue;
      moveButton(&entries[i].button, x, FOOTER_Y);
      drawButton(&entries[i].button);
      x += getButtonWidth(&entries[i].button);
      if (i + 1 < entryCount) x += FOOTER_GROUP_GAP;
   }
}
