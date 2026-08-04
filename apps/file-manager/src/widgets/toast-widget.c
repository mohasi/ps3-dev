// toast-widget - see toast-widget.h
#include "widgets/toast-widget.h"
#include "ui/label.h"
#include "theme.h"
#include "gfx.h"
#include <sys/sys_time.h>

#define TOAST_SECONDS      5
#define TOAST_TEXT_SIZE   24
#define TOAST_PAD_X       28
#define TOAST_PAD_Y       14
#define TOAST_BOTTOM_GAP 120   // clear of the footer button row

static Label label;
static uint64_t hideAtMicroseconds;   // 0 while nothing is showing

void initToastWidget(Font *font)
{
   initLabelRaw(&label, font, 0, 0, AUTO, AUTO, TOAST_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, NULL);
   hideAtMicroseconds = 0;
}

void showToast(const char *message)
{
   setLabelText(&label, message);
   hideAtMicroseconds = sys_time_get_system_time() + (uint64_t)TOAST_SECONDS * 1000000;
}

void updateToastWidget(void)
{
   if (hideAtMicroseconds && sys_time_get_system_time() >= hideAtMicroseconds) hideAtMicroseconds = 0;
}

void rethemeToastWidget(void)
{
   setLabelColor(&label, activeTheme->textPrimary);
}

void drawToastWidget(void)
{
   if (!hideAtMicroseconds) return;

   int boxWidth  = label.tt.tex.w + 2 * TOAST_PAD_X;
   int boxHeight = label.tt.tex.h + 2 * TOAST_PAD_Y;
   int boxX      = (getGfxScreenWidth() - boxWidth) / 2;
   int boxY      = getGfxScreenHeight() - TOAST_BOTTOM_GAP - boxHeight;

   drawGfxBox(boxX, boxY, boxWidth, boxHeight, activeTheme->borderThickness, activeTheme->dialogFill, activeTheme->dialogBorder);
   drawLabelAt(&label, boxX + TOAST_PAD_X, boxY + TOAST_PAD_Y);
}

void termToastWidget(void)
{
   freeLabel(&label);
}
