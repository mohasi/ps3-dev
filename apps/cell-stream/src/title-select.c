// title-select - see title-select.h.
//
// A grid of box art. Only the tiles on screen hold a rasterised name, because there are around a
// hundred games and holding a line of text for each would cost far more than redrawing the fourteen
// that are visible.
//
// Searching narrows the grid rather than opening a screen of its own: the matches are kept as a
// list of positions into the full list, and everything below draws through that list, so with no
// search in force it is simply every game in order.

#include "title-select.h"

#include "app.h"
#include "colors.h"
#include "gfx.h"
#include "pad.h"
#include "printf.h"
#include "screen-chrome.h"
#include "string-utilities.h"
#include "osk-input.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"
#include "ui/label.h"
#include "ui/thumbnail-loader.h"
#include "xcloud-api.h"
#include "xcloud-catalog.h"
#include "xcloud-channels.h"

#define COLUMNS      7
#define ROWS_SHOWN   2
// the store's posters are 2:3, measured off a captured frame, so the tile is cut to the same shape
// and the picture fills it. two rows is the most that leaves the art a usable size.
#define TILE_WIDTH   228
#define TILE_ART     342
#define TILE_GAP     18
#define NAME_SIZE    20
#define NAME_GAP     8
#define HINT_SIZE    22
#define GLYPH_HEIGHT 28
#define HINT_BOTTOM  70   // how far the hint row sits above the bottom of the screen

#define TILE_FILL   0xFF141414
#define TILE_BORDER 0xFF3A3A3A

#define SEARCH_MAX 48

static const char *TITLE = "Choose a game";

static ThumbnailLoader thumbnails;

// which games the search has left, as positions into the full list
static int matches[TITLE_LIST_MAX];
static int matchCount;
static char search[SEARCH_MAX];

static const char *getTileArtUrl(int index, void *user)
{
   (void)user;
   return getXcloudTitleArtUrl(index);
}

// case insensitive, and anywhere in the name rather than only at the start: a word from the middle
// of a title is remembered as often as the first one
static int nameContains(const char *name, const char *wanted)
{
   for (int start = 0; name[start]; start++) {
      int at = 0;
      while (wanted[at] && name[start + at] && toLowerChar(name[start + at]) == toLowerChar(wanted[at])) at++;
      if (!wanted[at]) return 1;
   }
   return 0;
}

static void applySearch(void)
{
   matchCount = 0;
   for (int index = 0; index < getXcloudTitleCount(); index++)
      if (!search[0] || nameContains(getXcloudTitleName(index), search)) matches[matchCount++] = index;
}

// the console's own keyboard hands back the whole line at once when it is closed
static void onSearchDone(const char *text)
{
   strCopy(search, sizeof search, text ? text : "");
   applySearch();
}

static void drawTile(Label *name, int index, int x, int y, int chosen)
{
   fillGfxRectangle(x, y, TILE_WIDTH, TILE_ART, TILE_FILL);

   // the store's pictures are not all the same shape, so each is fitted inside the tile at its own
   // proportions and centred. stretching every one to the tile would distort most of them.
   GfxTexture art = getThumbnail(&thumbnails, index);
   if (art.w > 0 && art.h > 0) {
      int width = TILE_WIDTH, height = art.h * TILE_WIDTH / art.w;
      if (height > TILE_ART) { height = TILE_ART; width = art.w * TILE_ART / art.h; }

      drawGfxTexture(x + (TILE_WIDTH - width) / 2, y + (TILE_ART - height) / 2, width, height, art, 0, 0, 1, 1,
                     COLOR_WHITE, GFX_FILTER_LINEAR);
   }

   strokeGfxRectangle(x, y, TILE_WIDTH, TILE_ART, chosen ? 3 : 1, chosen ? COLOR_WHITE : TILE_BORDER);

   // a long name would run into the tile beside it, so it is cut down until it fits and given an
   // ellipsis to say so
   char shown[CATALOG_NAME_MAX];
   strCopy(shown, sizeof shown, getXcloudTitleName(index));
   for (int length = getStrLen(shown); length > 3 && measureFontText(name->font, NAME_SIZE, shown) > TILE_WIDTH;
        length--) {
      shown[length - 1] = 0;
      shown[length - 2] = '.';
      shown[length - 3] = '.';
   }

   setLabelColor(name, chosen ? COLOR_WHITE : TEXT_DIM);
   setLabelText(name, shown);
   drawLabelCentered(name, x + TILE_WIDTH / 2, y + TILE_ART + NAME_GAP);
}

// keeps the highlighted row on screen, moving the window only when it would leave
static int getFirstRow(int highlighted, int rowsShown)
{
   int rows = (matchCount + COLUMNS - 1) / COLUMNS;

   int first = highlighted / COLUMNS - rowsShown / 2;
   if (first > rows - rowsShown) first = rows - rowsShown;
   if (first < 0) first = 0;
   return first;
}

static int step(int highlighted, int by)
{
   int moved = highlighted + by;
   if (moved < 0) return 0;
   if (moved >= matchCount) return matchCount - 1;
   return moved;
}

int runTitleSelectScreen(Font *font)
{
   if (getXcloudTitleCount() <= 0) return -1;

   search[0] = 0;
   applySearch();
   startThumbnails(&thumbnails, getXcloudTitleCount(), getTileArtUrl, 0);

   Label title, position, name;
   initLabelRaw(&title, font, 0, 0, AUTO, AUTO, TITLE_SIZE, COLOR_WHITE, TEXT_NOWRAP, TITLE);
   initLabelRaw(&position, font, 0, 0, AUTO, AUTO, HINT_SIZE, TEXT_DIM, TEXT_NOWRAP, "");
   initLabelRaw(&name, font, 0, 0, AUTO, AUTO, NAME_SIZE, TEXT_DIM, TEXT_NOWRAP, "");

   loadConsoleGlyphs();

   ButtonHints hints;
   initButtonHints(&hints, font, getGfxScreenHeight() - HINT_BOTTOM, GLYPH_HEIGHT, HINT_SIZE, TEXT_DIM);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS), "Start");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Search");
   int resolutionHint = addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE), "720p");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE), "Back");

   int highlighted = 0;
   int chosen = -1;
   int deciding = 1;

   while (deciding && !appExitRequested) {
      appPoll();
      updatePad();
      takeLoadedThumbnails(&thumbnails);

      // the console's keyboard takes over the screen while it is up, so nothing here reads the
      // buttons or draws underneath it
      if (oskInputActive()) {
         if (highlighted >= matchCount) highlighted = matchCount > 0 ? matchCount - 1 : 0;
      } else {
         if (isPadButtonPressed(PAD_BTN_LEFT))  highlighted = step(highlighted, -1);
         if (isPadButtonPressed(PAD_BTN_RIGHT)) highlighted = step(highlighted, 1);
         if (isPadButtonPressed(PAD_BTN_UP))    highlighted = step(highlighted, -COLUMNS);
         if (isPadButtonPressed(PAD_BTN_DOWN))  highlighted = step(highlighted, COLUMNS);
         if (isPadButtonPressed(PAD_BTN_L1))    highlighted = step(highlighted, -COLUMNS * ROWS_SHOWN);
         if (isPadButtonPressed(PAD_BTN_R1))    highlighted = step(highlighted, COLUMNS * ROWS_SHOWN);

         if (isPadButtonPressed(PAD_BTN_SQUARE))
            setXcloudStreamHeight(getXcloudStreamHeight() == 720 ? 1080 : 720);

         if (isPadButtonPressed(PAD_BTN_TRIANGLE)) oskInputBegin("Search games", search, onSearchDone);
         if (isPadButtonPressed(PAD_BTN_CROSS) && matchCount > 0) { chosen = matches[highlighted]; deciding = 0; }
         if (isPadButtonPressed(PAD_BTN_CIRCLE)) deciding = 0;
      }

      beginGfxFrame();
      clearGfx(COLOR_BLACK);

      int centerX = getGfxScreenWidth() / 2;
      drawLabelCentered(&title, centerX, TITLE_Y);

      int gridWidth = COLUMNS * TILE_WIDTH + (COLUMNS - 1) * TILE_GAP;
      int rowHeight = TILE_ART + NAME_GAP + NAME_SIZE + TILE_GAP;
      int left = centerX - gridWidth / 2;
      int top = TITLE_Y + TITLE_SIZE + TILE_GAP * 2;

      int firstRow = getFirstRow(highlighted, ROWS_SHOWN);
      for (int row = 0; row < ROWS_SHOWN; row++) {
         for (int column = 0; column < COLUMNS; column++) {
            int slot = (firstRow + row) * COLUMNS + column;
            if (slot >= matchCount) continue;

            drawTile(&name, matches[slot], left + column * (TILE_WIDTH + TILE_GAP), top + row * rowHeight,
                     slot == highlighted);
         }
      }

      int below = top + ROWS_SHOWN * rowHeight;

      char line[128];
      if (search[0]) snprintf(line, sizeof line, "Search \"%s\"    %d of %d", search,
                              matchCount ? highlighted + 1 : 0, matchCount);
      else snprintf(line, sizeof line, "%d of %d", highlighted + 1, matchCount);
      setLabelText(&position, line);
      drawLabelCentered(&position, centerX, below);

      snprintf(line, sizeof line, "%dp", getXcloudStreamHeight());
      setButtonHintCaption(&hints, resolutionHint, line);
      drawButtonHints(&hints, getGfxScreenWidth());

      endGfxFrame();
   }

   freeThumbnails(&thumbnails);
   termButtonHints(&hints);
   freeLabel(&title);
   freeLabel(&position);
   freeLabel(&name);
   return chosen;
}
