// toast - a self-dismissing top-right message (see toast.h). one Label plus a
// timestamp; it holds fully visible for a moment, then fades out over ~half a second.

#include "toast.h"
#include "ui/label.h"
#include "gfx.h"
#include "colors.h"
#include "net-common.h"   // getTimeUs

#define TOAST_SIZE       26
#define TOAST_MARGIN     30
#define TOAST_PADDING    12
#define TOAST_HOLD_US    1600000   // fully visible
#define TOAST_FADE_US    500000    // then fades out
#define TOAST_PANEL_MAX  0xC0       // panel opacity at full visibility (of 0xFF)

static Font  *toastFont;
static Label  toastLabel;
static uint64_t shownUs;      // when the current toast appeared; 0 = never shown
static int    active;         // still within its lifetime
static int    panelX, panelY, panelWidth, panelHeight;

void initToast(Font *font)
{
   toastFont = font;
   initLabelRaw(&toastLabel, font, 0, 0, AUTO, AUTO, TOAST_SIZE, COLOR_WHITE, TEXT_NOWRAP, "");
}

void showToast(const char *message)
{
   setLabelText(&toastLabel, message);

   int textWidth = (int)measureFontText(toastFont, TOAST_SIZE, message);
   panelWidth = textWidth + TOAST_PADDING * 2;
   panelHeight = TOAST_SIZE + TOAST_PADDING * 2;
   panelX = getGfxScreenWidth() - panelWidth - TOAST_MARGIN;
   panelY = TOAST_MARGIN;
   toastLabel.x = panelX + TOAST_PADDING;
   toastLabel.y = panelY + TOAST_PADDING;

   shownUs = getTimeUs();
   active = 1;
}

// 0..255 for how visible the toast is right now; 0 once its lifetime is over
static int currentAlpha(void)
{
   uint64_t elapsed = getTimeUs() - shownUs;
   if (elapsed < TOAST_HOLD_US) return 255;
   if (elapsed < TOAST_HOLD_US + TOAST_FADE_US) return 255 - (int)((elapsed - TOAST_HOLD_US) * 255 / TOAST_FADE_US);
   return 0;
}

void updateToast(void)
{
   if (active && currentAlpha() == 0) active = 0;
}

void drawToast(void)
{
   if (!active) return;
   int alpha = currentAlpha();

   int panelAlpha = alpha * TOAST_PANEL_MAX / 255;
   fillGfxRectangle(panelX, panelY, panelWidth, panelHeight, (uint32_t)panelAlpha << 24);
   drawLabelAlpha(&toastLabel, alpha);
}

void freeToast(void)
{
   freeLabel(&toastLabel);
}
