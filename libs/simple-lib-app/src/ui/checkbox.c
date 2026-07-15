// checkbox - draws the flat checkbox from icon-font glyphs (see ui/checkbox.h).
#include "ui/checkbox.h"
#include "ui/icon-font.h"

#define CHECKBOX_GLYPH_SIZE 25   // the checkbox font size

static Icon emptyIcon, checkedIcon;
static int  ready;

void initCheckboxIcons(void)
{
   if (ready) return;
   initIcon(&emptyIcon,   ICON_CHECK_EMPTY, CHECKBOX_GLYPH_SIZE);   // empty box
   initIcon(&checkedIcon, ICON_CHECK,       CHECKBOX_GLYPH_SIZE);   // box + tick (the tick overhangs the box)
   ready = 1;
}

void freeCheckboxIcons(void)
{
   if (!ready) return;
   freeIcon(&emptyIcon);
   freeIcon(&checkedIcon);
   ready = 0;
}

void drawCheckboxAlpha(Checkbox *cb, int isChecked, int alpha)
{
   // the two glyphs share the same square in the font, so drawing either at the same anchor keeps the box
   // identical and lets the checked glyph's tick overhang naturally - no scaling, no squashing.
   Icon *icon = isChecked ? &checkedIcon : &emptyIcon;
   drawIconAlpha(icon, cb->x, cb->y + 1, cb->color, alpha);
}
