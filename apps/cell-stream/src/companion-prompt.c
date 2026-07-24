// companion-prompt - see companion-prompt.h. a fixed title near the top, then the "server needed"
// message, a QR code to the release page, and the URL, sat lower down so the shortcut bar has room.
// positions are fixed for the console's 1080p output.

#include "companion-prompt.h"
#include "ui/label.h"
#include "gfx.h"
#include "colors.h"
#include "qr-code.h"

#define TITLE_Y      90          // the big title stays put near the top (matches the bottom bar's margin)
#define TITLE_SIZE   48
#define HINT_SIZE    24
#define URL_SIZE     26
#define QR_GAP       48          // breathing room between the QR and the text above/below it

#define MODULE_PIXELS 10
#define QUIET_MODULES 4          // light border so the code scans
#define QR_PANEL      (QR_SIZE * MODULE_PIXELS + 2 * QUIET_MODULES * MODULE_PIXELS)

#define TEXT_DIM  0xFFB0B0B0

static const char *TITLE = "Waiting for server...";
static const char *HINT  = "To start, run the companion server on Windows®. Don't have it? Scan the code:";
static const char *URL   = "codeberg.org/mohasi/ps3-dev/releases/latest";

static Font *promptFont;
static Label title, hint, url;

void initCompanionPrompt(Font *font)
{
   promptFont = font;
   initLabelRaw(&title, font, 0, 0, AUTO, AUTO, TITLE_SIZE, COLOR_WHITE, TEXT_NOWRAP, TITLE);
   initLabelRaw(&hint, font, 0, 0, AUTO, AUTO, HINT_SIZE, TEXT_DIM, TEXT_NOWRAP, HINT);
   initLabelRaw(&url, font, 0, 0, AUTO, AUTO, URL_SIZE, COLOR_WHITE, TEXT_NOWRAP, URL);
}

// a white quiet-zone panel with the dark modules drawn on top, centred on centerX at topY
static void drawQr(int centerX, int topY)
{
   int panelX = centerX - QR_PANEL / 2;
   fillGfxRectangle(panelX, topY, QR_PANEL, QR_PANEL, COLOR_WHITE);

   int originX = panelX + QUIET_MODULES * MODULE_PIXELS;
   int originY = topY + QUIET_MODULES * MODULE_PIXELS;
   for (int row = 0; row < QR_SIZE; row++)
      for (int col = 0; col < QR_SIZE; col++)
         if (QR_MODULES[row * QR_SIZE + col])
            fillGfxRectangle(originX + col * MODULE_PIXELS, originY + row * MODULE_PIXELS, MODULE_PIXELS, MODULE_PIXELS, COLOR_BLACK);
}

static void drawCenteredLabel(Label *label, int size, const char *text, int centerX, int y)
{
   label->x = centerX - (int)measureFontText(promptFont, size, text) / 2;
   label->y = y;
   drawLabel(label);
}

void drawCompanionPrompt(void)
{
   int centerX = getGfxScreenWidth() / 2;

   drawCenteredLabel(&title, TITLE_SIZE, TITLE, centerX, TITLE_Y);

   // the message + QR + url sit as one block, centred vertically on the screen
   int blockHeight = HINT_SIZE + QR_GAP + QR_PANEL + QR_GAP + URL_SIZE;
   int y = (getGfxScreenHeight() - blockHeight) / 2;
   drawCenteredLabel(&hint, HINT_SIZE, HINT, centerX, y);
   y += HINT_SIZE + QR_GAP;
   drawQr(centerX, y);
   y += QR_PANEL + QR_GAP;
   drawCenteredLabel(&url, URL_SIZE, URL, centerX, y);
}

void freeCompanionPrompt(void)
{
   freeLabel(&title);
   freeLabel(&hint);
   freeLabel(&url);
}
