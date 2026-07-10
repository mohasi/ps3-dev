// home - the landing screen: trending categories in a VideoGrid (see home.h).

#include "screens/home.h"
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
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MARGIN_X      50
#define HEADER_Y      26
#define TITLE_SIZE    26
#define GRID_TOP      74
#define HINTS_BOTTOM  42        // hint-row top offset from the screen bottom (overlaid, no layout shift)
#define HINT_GLYPH_H  22
#define HINT_TEXT     18

static const char *TrendingNames[TREND_COUNT] = { "Gaming", /* "Music", */ "Live", "Sports", "Podcasts" };

// home categories = the trending feeds plus a Subscriptions feed and the Watch Later queue.
#define HOME_SUBS        TREND_COUNT
#define HOME_WATCHLATER  (TREND_COUNT + 1)
#define HOME_CAT_COUNT   (TREND_COUNT + 2)
#define SUBS_PER_CHANNEL 8      // latest videos pulled from each subscribed channel

// each category's fully-loaded results, so returning to a tab skips the feed fetch (thumbnails still refetch).
static SearchResults categoryCache[HOME_CAT_COUNT];
static int           categoryCached[HOME_CAT_COUNT];
static int           subsRevisionSeen;         // subscriptions revision the cached HOME_SUBS feed was built at
static int           watchLaterRevisionSeen;   // watch-later revision the cached HOME_WATCHLATER feed was built at

static struct {
   VideoGrid grid;
   int       category;
   int       screenW, screenH;
} home;

static Font        font;
static Label       titleLabel, statusLabel;
static ButtonHints hints;

// approximate seconds-ago from a "N unit ago" string. approximate is fine here (unlike a precise date sort):
// it only needs to group recent above old across channels. unknown/live sorts last.
#define AGE_UNKNOWN 2000000000L   // larger than any real age, still inside a 32-bit long

static long ageSeconds(const char *published)
{
   const char *digit = published;
   while (*digit && (*digit < '0' || *digit > '9')) digit++;
   if (!*digit) return AGE_UNKNOWN;
   long value = atol(digit);
   if      (strstr(published, "second")) return value;
   else if (strstr(published, "minute")) return value * 60;
   else if (strstr(published, "hour"))   return value * 3600;
   else if (strstr(published, "day"))    return value * 86400;
   else if (strstr(published, "week"))   return value * 604800;
   else if (strstr(published, "month"))  return value * 2592000;
   else if (strstr(published, "year"))   return value * 31536000;
   return AGE_UNKNOWN;
}

static int byRecency(const void *a, const void *b)
{
   long ageA = ageSeconds(((const SearchResult *)a)->published), ageB = ageSeconds(((const SearchResult *)b)->published);
   return (ageA > ageB) - (ageA < ageB);
}

// aggregate the latest videos from each subscribed channel into one feed, newest first. bails if the source
// was swapped away mid-fetch (each channel is a separate blocking request), so switching off stays snappy.
static int fetchSubscriptions(SearchResults *out)
{
   char channels[MAX_SUBSCRIPTIONS][CHANNEL_ID_LEN];
   int channelCount = getSubscriptions(channels, MAX_SUBSCRIPTIONS);
   memset(out, 0, sizeof *out);
   if (channelCount == 0) return -1;

   SearchResults *page = (SearchResults *)malloc(sizeof *page);
   if (!page) return -1;
   for (int c = 0; c < channelCount && out->count < MAX_SEARCH_RESULTS; c++) {
      if (gridStopRequested(&home.grid)) break;
      if (getChannelVideos(channels[c], NULL, page) != 0) continue;   // NULL token = the channel's latest page
      for (int i = 0, added = 0; i < page->count && added < SUBS_PER_CHANNEL && out->count < MAX_SEARCH_RESULTS; i++) {
         int duplicate = 0;
         for (int j = 0; j < out->count; j++)
            if (strcmp(out->items[j].videoId, page->items[i].videoId) == 0) { duplicate = 1; break; }
         if (duplicate) continue;
         // channel-page items omit their own channel id/name (the channel is implied) - stamp them so the
         // tile shows the channel and Triangle can open it.
         SearchResult *item = &out->items[out->count++];
         *item = page->items[i];
         strCopy(item->channelId, sizeof item->channelId, channels[c]);
         if (!item->author[0]) strCopy(item->author, sizeof item->author, page->channelName);
         added++;
      }
   }
   free(page);
   qsort(out->items, out->count, sizeof out->items[0], byRecency);   // mixed recency across all channels
   out->continuation[0] = 0;   // merged feed - no single continuation to page (for now)
   return out->count > 0 ? 0 : -1;
}

static int homeFetch(const char *token, SearchResults *out, void *user)
{
   (void)user;
   if (home.category == HOME_SUBS)       return fetchSubscriptions(out);
   if (home.category == HOME_WATCHLATER) return getWatchLater(out) > 0 ? 0 : -1;
   return getTrending(home.category, token, out);
}

static void setHeading(void)
{
   char heading[128];
   if      (home.category == HOME_SUBS)       snprintf(heading, sizeof heading, "Subscriptions");
   else if (home.category == HOME_WATCHLATER) snprintf(heading, sizeof heading, "Watch Later");
   else                                       snprintf(heading, sizeof heading, "Trending \xe2\x80\xa2 %s", TrendingNames[home.category]);
   setLabelText(&titleLabel, heading);
}

static void loadCategory(void)
{
   setHeading();
   if (categoryCached[home.category]) setGridCached(&home.grid, &categoryCache[home.category], homeFetch, NULL);
   else                               setGridSource(&home.grid, homeFetch, NULL);
}

// cache the current category (only when it's fully loaded) so returning to it is instant.
static void cacheCurrent(void)
{
   if (gridStage(&home.grid) != GRID_READY || gridBusy(&home.grid)) return;   // don't copy mid-append
   categoryCache[home.category] = *gridResults(&home.grid);
   categoryCached[home.category] = 1;
}

static void switchCategory(int delta)
{
   cacheCurrent();
   home.category = (home.category + delta + HOME_CAT_COUNT) % HOME_CAT_COUNT;
   loadCategory();
}

// OSK confirmed: a link plays directly, anything else opens a search.
static void onQueryEntered(const char *text)
{
   if (!text || !text[0]) return;
   if (strstr(text, "youtu")) { stopVideoGrid(&home.grid); playVideo(text); return; }
   cacheCurrent();
   openSearchQuery(text);
}

static void initHome(void)
{
   home.screenW = getGfxScreenWidth();
   home.screenH = getGfxScreenHeight();

   font = openSystemFont(FONT_POP);
   initLabel(&titleLabel,  &font, MARGIN_X, HEADER_Y, home.screenW - 2 * MARGIN_X, AUTO, TITLE_SIZE, COLOR_AMBER_300, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&statusLabel, &font, MARGIN_X, GRID_TOP, home.screenW - 2 * MARGIN_X, AUTO, 21, COLOR_SLATE_100, TEXT_NOWRAP, "Loading...");

   initButtonHints(&hints, &font, home.screenH - HINTS_BOTTOM, HINT_GLYPH_H, HINT_TEXT, COLOR_SLATE_300);
   addButtonHint(&hints, getConsoleGlyph(GLYPH_CROSS),    "Play");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_SQUARE),   "Watch Later");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_TRIANGLE), "Channel");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_START),    "Search");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_L1),       "");
   addButtonHint(&hints, getConsoleGlyph(GLYPH_R1),       "Category");

   initVideoGrid(&home.grid, &font, MARGIN_X, GRID_TOP, home.screenW - 2 * MARGIN_X, home.screenH - GRID_TOP);
   setGridWatchedPredicate(&home.grid, isWatched);
   setGridWatchLaterPredicate(&home.grid, isWatchLater);

   subsRevisionSeen = getSubscriptionsRevision();
   watchLaterRevisionSeen = getWatchLaterRevision();
   home.category = HOME_SUBS;   // always start on Subscriptions
   loadCategory();
}

static void updateHome(void)
{
   if (oskInputActive()) return;

   if (isPadButtonPressed(PAD_BTN_START)) oskInputBegin("Search YouTube", "Dirt League", onQueryEntered);

   // L1/R1 stay responsive even mid-load (the source swap is non-blocking)
   if      (isPadButtonPressed(PAD_BTN_R1)) { switchCategory(1);  return; }
   else if (isPadButtonPressed(PAD_BTN_L1)) { switchCategory(-1); return; }

   updateVideoGrid(&home.grid);
   if (gridStage(&home.grid) != GRID_READY) return;

   const SearchResult *selected = gridSelected(&home.grid);
   if (!selected) return;
   if (isPadButtonPressed(PAD_BTN_SQUARE)) {
      toggleWatchLater(selected);
      watchLaterRevisionSeen = getWatchLaterRevision();
      categoryCached[HOME_WATCHLATER] = 0;
      if (home.category == HOME_WATCHLATER) loadCategory();   // reflect the add/remove immediately
      return;
   }
   if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { cacheCurrent(); openChannelFor(selected); return; }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      markWatched(selected->videoId);
      stopVideoGrid(&home.grid);   // no per-call http fetch may overlap the http media stream during playback
      playVideo(selected->videoId);
   }
}

static void drawHome(void)
{
   fillGfxRectangle(0, 0, home.screenW, home.screenH, COLOR_SLATE_900);
   drawLabel(&titleLabel);
   if (gridStage(&home.grid) == GRID_READY) {
      drawVideoGrid(&home.grid);
   } else {
      setLabelText(&statusLabel, gridStage(&home.grid) == GRID_EMPTY ? "No results" : "Loading...");
      drawLabel(&statusLabel);
   }
   drawButtonHints(&hints, home.screenW);
}

static void suspendHome(void) { stopVideoGrid(&home.grid); }   // free the http pools for the pushed screen

// returning from search/play: if the load was interrupted mid-flight (worker stopped), restart it; an
// already-loaded grid is kept as-is (instant, thumbnails intact).
static void resumeHome(void)
{
   // a subscribe/unsubscribe while we were away makes the cached Subscriptions feed stale - drop it and,
   // if it's the current tab, refetch now.
   if (getSubscriptionsRevision() != subsRevisionSeen) {
      subsRevisionSeen = getSubscriptionsRevision();
      categoryCached[HOME_SUBS] = 0;
      if (home.category == HOME_SUBS) { loadCategory(); return; }
   }
   if (getWatchLaterRevision() != watchLaterRevisionSeen) {
      watchLaterRevisionSeen = getWatchLaterRevision();
      categoryCached[HOME_WATCHLATER] = 0;
      if (home.category == HOME_WATCHLATER) { loadCategory(); return; }
   }
   if (gridStage(&home.grid) != GRID_READY) loadCategory();
}

static void termHome(void)
{
   termVideoGrid(&home.grid);
   termButtonHints(&hints);
   freeLabel(&titleLabel);
   freeLabel(&statusLabel);
   closeFont(&font);
}

void openHome(void) { changeScreen(&homeScreen); }

// Screen vtable order: init, resume, update, draw, suspend, term, status.
Screen homeScreen = { initHome, resumeHome, updateHome, drawHome, suspendHome, termHome, SCREEN_TERMINATED };
