// search screen - a VideoGrid in one of two modes: a text search or a channel's videos (see search.h).
// Pushed on top of home. Triangle drills into a channel in place, Circle backs out (channel -> search
// results -> pop to home), SELECT cycles the sort, Start re-searches, X plays.

#include "screens/search.h"
#include "screens/play.h"
#include "video-grid.h"
#include "storage.h"
#include "ui/button-hints.h"
#include "ui/console-glyphs.h"

#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "font.h"
#include "ui/label.h"
#include "screen-manager.h"
#include "osk-input.h"
#include "string-utilities.h"
#include <string.h>
#include <stdio.h>

#define MARGIN_X      50
#define HEADER_Y      26
#define TITLE_SIZE    26
#define GRID_TOP      74
#define HINTS_BOTTOM  42
#define HINT_GLYPH_H  22
#define HINT_TEXT     18
#define TRIANGLE_HINT 3         // Triangle's slot in the hint row (see initSearch): retitled per mode

static const char *SortNames[SORT_COUNT]                = { "Relevance", "Views" };
static const char *ChannelSortNames[CHANNEL_SORT_COUNT] = { "Latest", "Popular", "Oldest" };

// how the next push opens (set by openSearchQuery / openChannelFor before pushScreen).
static char         pendingQuery[128];
static SearchResult pendingChannel;
static int          pendingIsChannel;

static struct {
   VideoGrid grid;
   char      query[128];
   char      channelId[32];
   char      channelName[48];
   char      channelSortToken[MAX_SORT_TOKEN];   // first-page token for the chosen channel sort ("" = Latest)
   int       channelSort;
   int       sort;
   int       screenW, screenH;
} search;

static Font        font;
static Label       titleLabel, sortLabel, statusLabel;
static ButtonHints hints;

static int searchFetch(const char *token, SearchResults *out, void *user)
{
   (void)user;
   if (search.channelId[0]) {
      const char *first = search.channelSortToken[0] ? search.channelSortToken : NULL;
      return getChannelVideos(search.channelId, token ? token : first, out);
   }
   return searchVideos(search.query, search.sort, token, out);
}

// in channel mode Triangle toggles the subscription (you're already in the channel); elsewhere it drills in.
static void updateTriangleHint(void)
{
   const char *caption = "Channel";
   if (search.channelId[0]) caption = isSubscribed(search.channelId) ? "Unsubscribe" : "Subscribe";
   setButtonHintCaption(&hints, TRIANGLE_HINT, caption);
}

// title (left) shows the mode; sortLabel (right) shows the current ordering.
static void setHeading(void)
{
   char heading[192];
   if (search.channelId[0]) { snprintf(heading, sizeof heading, "%s", search.channelName); setLabelText(&sortLabel, ChannelSortNames[search.channelSort]); }
   else                     { snprintf(heading, sizeof heading, "Search: %s", search.query); setLabelText(&sortLabel, SortNames[search.sort]); }
   setLabelText(&titleLabel, heading);
   updateTriangleHint();
}

static void reload(void)
{
   setHeading();
   setGridSource(&search.grid, searchFetch, NULL);
}

static void enterChannel(const SearchResult *item)
{
   if (!item->channelId[0]) return;
   strCopy(search.channelId, sizeof search.channelId, item->channelId);
   strCopy(search.channelName, sizeof search.channelName, item->author[0] ? item->author : "Channel");
   search.channelSort = CHANNEL_SORT_LATEST;
   search.channelSortToken[0] = 0;
   reload();
}

static void cycleChannelSort(void)
{
   const SearchResults *results = gridResults(&search.grid);
   if (!results->sortTokens[CHANNEL_SORT_LATEST][0]) return;   // chips not loaded yet
   search.channelSort = (search.channelSort + 1) % CHANNEL_SORT_COUNT;
   strCopy(search.channelSortToken, sizeof search.channelSortToken, results->sortTokens[search.channelSort]);
   reload();
}

static void onQueryEntered(const char *text)
{
   if (!text || !text[0]) return;
   if (strstr(text, "youtu")) { stopVideoGrid(&search.grid); playVideo(text); return; }
   search.channelId[0] = 0;   // a search exits channel mode
   strCopy(search.query, sizeof search.query, text);
   search.sort = SORT_RELEVANCE;
   reload();
}

static void initSearch(void)
{
   search.screenW = getGfxScreenWidth();
   search.screenH = getGfxScreenHeight();

   memset(search.channelId, 0, sizeof search.channelId);
   search.query[0] = 0;
   search.sort = SORT_RELEVANCE;
   if (pendingIsChannel) {
      strCopy(search.channelId, sizeof search.channelId, pendingChannel.channelId);
      strCopy(search.channelName, sizeof search.channelName, pendingChannel.author[0] ? pendingChannel.author : "Channel");
      search.channelSort = CHANNEL_SORT_LATEST;
      search.channelSortToken[0] = 0;
   } else {
      strCopy(search.query, sizeof search.query, pendingQuery);
   }

   font = openSystemFont(FONT_POP);
   initLabel(&titleLabel,  &font, MARGIN_X, HEADER_Y, search.screenW - 300, AUTO, TITLE_SIZE, COLOR_AMBER_300, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&sortLabel,   &font, 0, HEADER_Y, AUTO, AUTO, TITLE_SIZE, COLOR_SLATE_300, TEXT_NOWRAP, "");
   initLabel(&statusLabel, &font, MARGIN_X, GRID_TOP, search.screenW - 2 * MARGIN_X, AUTO, 21, COLOR_SLATE_100, TEXT_NOWRAP, "Loading...");

   initButtonHints(&hints, &font, search.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, COLOR_SLATE_300);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CIRCLE),   "Back");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SELECT),   "Sort");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START),    "Search");

   initVideoGrid(&search.grid, &font, MARGIN_X, GRID_TOP, search.screenW - 2 * MARGIN_X, search.screenH - GRID_TOP);
   setGridWatchedPredicate(&search.grid, isWatched);
   setGridWatchLaterPredicate(&search.grid, isWatchLater);
   reload();
}

static void updateSearch(void)
{
   if (oskInputActive()) return;

   if (isPadButtonPressed(PAD_BTN_START)) { oskInputBegin("Search YouTube", search.query[0] ? search.query : "Dirt League", onQueryEntered); }

   // Circle: channel-with-underlying-search -> back to results; otherwise pop to home
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      if (search.channelId[0] && search.query[0]) { search.channelId[0] = 0; reload(); }
      else popScreen();
      return;
   }
   if (isPadButtonPressed(PAD_BTN_SELECT)) {
      if (search.channelId[0]) cycleChannelSort();
      else { search.sort = (search.sort + 1) % SORT_COUNT; reload(); }
      return;
   }

   updateVideoGrid(&search.grid);
   if (gridStage(&search.grid) != GRID_READY) return;

   const SearchResult *selected = gridSelected(&search.grid);
   if (!selected) return;
   if (isPadButtonPressed(PAD_BTN_SQUARE)) { toggleWatchLater(selected); return; }
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) {
      if (search.channelId[0]) { setSubscribed(search.channelId, !isSubscribed(search.channelId)); updateTriangleHint(); }
      else enterChannel(selected);
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatched(selected->videoId);
      stopVideoGrid(&search.grid);
      playVideo(selected->videoId);
   }
}

static void drawSearch(void)
{
   fillGfxRectangle(0, 0, search.screenW, search.screenH, COLOR_SLATE_900);
   drawLabel(&titleLabel);
   moveLabel(&sortLabel, search.screenW - MARGIN_X - sortLabel.tt.tex.w, HEADER_Y);   // right-aligned
   drawLabel(&sortLabel);

   if (gridStage(&search.grid) == GRID_READY) {
      drawVideoGrid(&search.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&search.grid) == GRID_EMPTY ? "No results" : "Loading...");
      drawLabel(&statusLabel);
   }

   drawButtonHints(&hints, search.screenW);
}

static void termSearch(void)
{
   termVideoGrid(&search.grid);
   termButtonHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&sortLabel);
   freeLabel(&statusLabel);
   closeFont(&font);
}

void openSearchQuery(const char *query)
{
   strCopy(pendingQuery, sizeof pendingQuery, query);
   pendingIsChannel = 0;
   pushScreen(&searchScreen);
}

void openChannelFor(const SearchResult *item)
{
   pendingChannel = *item;
   pendingIsChannel = 1;
   pushScreen(&searchScreen);
}

Screen searchScreen = { initSearch, NULL, updateSearch, drawSearch, NULL, termSearch, SCREEN_TERMINATED };
