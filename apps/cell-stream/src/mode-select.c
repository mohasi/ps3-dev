// mode-select - see mode-select.h. two stacked tiles, centred, driven by up/down and Cross.
// positions are fixed for the console's 1080p output, matching companion-prompt.

#include "mode-select.h"

#include "app.h"
#include "colors.h"
#include "gfx.h"
#include "pad.h"
#include "screen-chrome.h"
#include "ui/label.h"

#define TILE_WIDTH     720
#define TILE_HEIGHT    132
#define TILE_GAP       32
#define TILE_TEXT_SIZE 34

#define TILE_FILL          0xFF141414
#define TILE_FILL_CHOSEN   0xFF1F1F1F
#define TILE_BORDER        0xFF3A3A3A

static const char *TITLE = "cell-stream";

static const char *sourceNames[STREAM_SOURCE_COUNT] = { "PC Streaming", "Xbox Cloud" };

static void drawTile(Label *label, int centerX, int y, int chosen)
{
   int tileX = centerX - TILE_WIDTH / 2;
   fillGfxRectangle(tileX, y, TILE_WIDTH, TILE_HEIGHT, chosen ? TILE_FILL_CHOSEN : TILE_FILL);
   strokeGfxRectangle(tileX, y, TILE_WIDTH, TILE_HEIGHT, chosen ? 3 : 1, chosen ? COLOR_WHITE : TILE_BORDER);
   drawLabelCentered(label, centerX, y + (TILE_HEIGHT - TILE_TEXT_SIZE) / 2);
}

StreamSource runModeSelectScreen(Font *font)
{
   Label title, tileLabels[STREAM_SOURCE_COUNT];
   initLabelRaw(&title, font, 0, 0, AUTO, AUTO, TITLE_SIZE, COLOR_WHITE, TEXT_NOWRAP, TITLE);
   for (int source = 0; source < STREAM_SOURCE_COUNT; source++)
      initLabelRaw(&tileLabels[source], font, 0, 0, AUTO, AUTO, TILE_TEXT_SIZE, TEXT_DIM, TEXT_NOWRAP,
                   sourceNames[source]);

   StreamSource chosen = STREAM_SOURCE_NONE;
   int highlighted = STREAM_SOURCE_PC;
   int deciding = 1;

   while (deciding && !appExitRequested) {
      appPoll();
      updatePad();   // no pad thread yet: this screen is the only reader

      if (isPadButtonPressed(PAD_BTN_UP)) highlighted = (highlighted + STREAM_SOURCE_COUNT - 1) % STREAM_SOURCE_COUNT;
      if (isPadButtonPressed(PAD_BTN_DOWN)) highlighted = (highlighted + 1) % STREAM_SOURCE_COUNT;
      if (isPadButtonPressed(PAD_BTN_CROSS)) { chosen = (StreamSource)highlighted; deciding = 0; }
      if (isPadButtonPressed(PAD_BTN_START)) deciding = 0;

      // colours change with the highlight, so they are set here rather than in the draw below:
      // setLabelColor re-rasterises the text, which the draw path must not do.
      for (int source = 0; source < STREAM_SOURCE_COUNT; source++)
         setLabelColor(&tileLabels[source], source == highlighted ? COLOR_WHITE : TEXT_DIM);

      beginGfxFrame();
      clearGfx(COLOR_BLACK);

      int centerX = getGfxScreenWidth() / 2;
      drawLabelCentered(&title, centerX, TITLE_Y);

      // the tiles sit as one block, centred vertically
      int blockHeight = STREAM_SOURCE_COUNT * TILE_HEIGHT + (STREAM_SOURCE_COUNT - 1) * TILE_GAP;
      int y = (getGfxScreenHeight() - blockHeight) / 2;
      for (int source = 0; source < STREAM_SOURCE_COUNT; source++) {
         drawTile(&tileLabels[source], centerX, y, source == highlighted);
         y += TILE_HEIGHT + TILE_GAP;
      }

      endGfxFrame();
   }

   freeLabel(&title);
   for (int source = 0; source < STREAM_SOURCE_COUNT; source++) freeLabel(&tileLabels[source]);
   return chosen;
}
