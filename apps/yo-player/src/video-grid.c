// video-grid - the reusable windowed video-result grid (see video-grid.h).
//
// A background worker fetches result pages (via the screen-supplied GridFetchFn) and thumbnail jpegs; the
// jpegs are kept and only the on-screen band of tiles is "materialised" (thumbnail decoded to a texture +
// labels rasterised), freed again on scroll-away and re-decoded from the kept jpeg on the way back, so VRAM
// stays flat no matter how many results are loaded. The worker is stopped before playback so no per-call
// http fetch overlaps the streaming http-fs client (they share libhttp's pools).

#include "video-grid.h"

#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "http-fetch.h"
#include "string-utilities.h"   // strCopy
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define GRID_GAP    22
#define TILE_TEXT_H 54          // space under each thumbnail for the title + meta line
#define ROW_TITLE   21
#define META_SIZE   15
#define DUR_SIZE    19
#define MARGIN_ROWS 2           // rows kept materialised above and below the visible band
#define THUMB_LOOKAHEAD_ROWS 3  // rows below the visible band to fetch thumbnails for ahead of scrolling
#define DECODES_PER_FRAME 3
#define THUMB_CAP   (96 * 1024) // one mqdefault jpeg fits easily
#define THUMB_UA    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

#define PLACEHOLDER 0xFF1E293B
#define BADGE_BG    0xCC000000
#define BADGE_LIVE  0xE0CC0000
#define WATCHED_TINT  0xFF4D4D4D // watched-tile thumbnail: opaque RGB darken (kept opaque so the amber
                                 // selection border can't bleed through the tile), heavily dimmed
#define WATCHED_ALPHA 70         // label alpha for already-watched tiles

// ---- thumbnail fetch (worker side) ----

static void thumbFetchThread(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   uint8_t *buffer = (uint8_t *)malloc(THUMB_CAP);
   if (buffer) {
      CellHttpHeader headers[] = { { "User-Agent", THUMB_UA }, { "Accept-Encoding", "identity" } };
      for (;;) {
         int i = __sync_fetch_and_add(&grid->thumbNext, 1);
         if (i >= grid->thumbEnd || grid->stopWorker) break;

         char url[128];
         snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", grid->results.items[i].videoId);
         int length = 0, status = 0;
         int rc = httpFetch(CELL_HTTP_METHOD_GET, url, headers, 2, NULL, 0, (char *)buffer, THUMB_CAP, &length, &status);
         if (rc == 0 && status == 200 && length > 0) {
            uint8_t *copy = (uint8_t *)malloc(length);
            if (copy) {
               memcpy(copy, buffer, length);
               grid->thumbs[i].jpeg = copy; grid->thumbs[i].jpegLen = length;
               __sync_synchronize();
               grid->thumbs[i].state = THUMB_FETCHED;
            }
         } else {
            grid->thumbs[i].state = THUMB_FAILED;
         }
      }
      free(buffer);
   }
   exitThread();
}

// fetch thumbnails for results [from, to) in parallel; returns once every one has landed or failed.
static void fetchThumbnailsRange(VideoGrid *grid, int from, int to)
{
   grid->thumbNext = from;
   grid->thumbEnd = to;
   sys_ppu_thread_t threads[GRID_THREADS];
   int spawned = 0;
   for (int t = 0; t < GRID_THREADS; t++)
      if (spawnJoinableThread(&threads[spawned], thumbFetchThread, (uint64_t)(uintptr_t)grid,
                              THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-thumb") == 0)
         spawned++;
   for (int t = 0; t < spawned; t++) joinThread(threads[t]);
}

// append a fetched page's items, skipping videoIds we already have. sortTokens (channel chips) are kept
// from the first page - continuation pages don't carry them.
static void appendPage(VideoGrid *grid, const SearchResults *page)
{
   for (int i = 0; i < page->count && grid->results.count < MAX_SEARCH_RESULTS; i++) {
      const SearchResult *item = &page->items[i];
      int duplicate = 0;
      for (int j = 0; j < grid->results.count; j++)
         if (strcmp(grid->results.items[j].videoId, item->videoId) == 0) { duplicate = 1; break; }
      if (duplicate) continue;
      grid->results.items[grid->results.count] = *item;
      __sync_synchronize();
      grid->results.count++;
   }
   strCopy(grid->results.continuation, sizeof grid->results.continuation, page->continuation);
}

// how far down to fetch thumbnails: the visible band plus a lookahead, clamped to what's loaded. thumbnails
// are fetched lazily to this frontier instead of all at once, so returning to a category is cheap.
static int thumbFrontier(const VideoGrid *grid)
{
   int frontier = (grid->scrollRow + grid->visibleRows + THUMB_LOOKAHEAD_ROWS) * GRID_COLS;
   if (frontier > grid->results.count) frontier = grid->results.count;
   return frontier;
}

static void worker(uint64_t arg)
{
   VideoGrid *grid = (VideoGrid *)(uintptr_t)arg;
   switch (grid->job) {
   case JOB_LOAD: {
      int rc = grid->fetch(NULL, &grid->results, grid->fetchUser);
      grid->stage = (rc == 0 && grid->results.count > 0) ? GRID_READY : GRID_EMPTY;
      __sync_synchronize();
      grid->searchParsed = 1;
      if (grid->stage == GRID_READY) { int target = thumbFrontier(grid); fetchThumbnailsRange(grid, 0, target); grid->thumbFetched = target; }
      break;
   }
   case JOB_CACHE: {   // results already restored; fetch only the first window of thumbnails
      int target = thumbFrontier(grid);
      fetchThumbnailsRange(grid, 0, target);
      grid->thumbFetched = target;
      break;
   }
   case JOB_THUMBS: {  // extend thumbnails to the frontier the window has scrolled to
      int target = thumbFrontier(grid);
      fetchThumbnailsRange(grid, grid->thumbFetched, target);
      grid->thumbFetched = target;
      break;
   }
   case JOB_MORE: {    // append the next page; its thumbnails are picked up lazily by a later JOB_THUMBS
      SearchResults *page = (SearchResults *)malloc(sizeof *page);
      if (page && grid->fetch(grid->results.continuation, page, grid->fetchUser) == 0) {
         int base = grid->results.count;
         appendPage(grid, page);
         if (grid->results.count == base) grid->results.continuation[0] = 0;   // nothing new: stop paging
      }
      free(page);
      break;
   }
   }
   __sync_synchronize();
   grid->workerDone = 1;
   exitThread();
}

static int spawnWorker(VideoGrid *grid)
{
   grid->stopWorker = 0;
   grid->workerDone = 0;
   grid->threadActive = (spawnJoinableThread(&grid->workerTid, worker, (uint64_t)(uintptr_t)grid,
                         THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_64KB, "yt-feed") == 0);
   return grid->threadActive;
}

// ---- tile windowing (UI side) ----

static void freeThumbnails(VideoGrid *grid)
{
   finishGfx();
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      if (grid->thumbs[i].jpeg) { free(grid->thumbs[i].jpeg); grid->thumbs[i].jpeg = NULL; }
      freeGfxTexture(&grid->thumbs[i].tex);
   }
   memset(grid->thumbs, 0, sizeof grid->thumbs);
}

static void resetFeed(VideoGrid *grid)
{
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
   memset(&grid->results, 0, sizeof grid->results);
   grid->stage = GRID_RUNNING;
   grid->searchParsed = grid->workerDone = 0;
   grid->thumbFetched = 0;
   grid->selected = grid->scrollRow = grid->winStart = grid->winEnd = 0;
}

static void beginFeed(VideoGrid *grid)
{
   if (grid->job == JOB_CACHE) {   // results already in hand: show immediately, worker only fetches thumbnails
      grid->stage = GRID_READY;
      __sync_synchronize();
      grid->searchParsed = 1;
   }
   if (!spawnWorker(grid)) {
      grid->workerDone = 1;
      if (grid->job != JOB_CACHE) { grid->stage = GRID_EMPTY; grid->searchParsed = 1; }
   }
}

// (re)load the current source: restore pendingCached results instantly if set (worker then only refetches
// thumbnails), else fetch page 1 fresh. run either immediately (idle) or deferred after the worker is reaped.
static void applyDeferredSource(VideoGrid *grid)
{
   resetFeed(grid);
   if (grid->pendingCached) { grid->results = *grid->pendingCached; grid->job = JOB_CACHE; }
   else                       grid->job = JOB_LOAD;
   beginFeed(grid);
}

static void loadMore(VideoGrid *grid)
{
   grid->job = JOB_MORE;
   if (!spawnWorker(grid)) grid->workerDone = 1;
}

// fetch the next band of thumbnails the window has scrolled toward (lazy, non-blocking)
static void loadThumbnails(VideoGrid *grid)
{
   grid->job = JOB_THUMBS;
   if (!spawnWorker(grid)) grid->workerDone = 1;
}

// join the non-empty metadata parts with " - " into "author - views - age".
static void composeMeta(char *out, int cap, const SearchResult *item)
{
   const char *parts[3] = { item->author, item->views, item->published };
   int length = 0;
   out[0] = 0;
   for (int i = 0; i < 3; i++) {
      if (!parts[i][0]) continue;
      length += snprintf(out + length, cap - length, "%s%s", length ? " \xe2\x80\xa2 " : "", parts[i]);
      if (length >= cap) break;
   }
}

static void materializeTile(VideoGrid *grid, int i)
{
   char meta[128];
   const SearchResult *item = &grid->results.items[i];
   setLabelText(&grid->rows[i], item->title);
   composeMeta(meta, sizeof meta, item);
   setLabelText(&grid->metas[i], meta);
   setLabelText(&grid->durations[i], item->isLive ? "LIVE" : item->duration);
}

static void dematerializeTile(VideoGrid *grid, int i)
{
   freeLabel(&grid->rows[i]);
   freeLabel(&grid->metas[i]);
   freeLabel(&grid->durations[i]);
   if (grid->thumbs[i].tex.offset) { freeGfxTexture(&grid->thumbs[i].tex); memset(&grid->thumbs[i].tex, 0, sizeof grid->thumbs[i].tex); }
}

// keep exactly the visible band (+ margin) materialised. safe without an RSX flush: a tile leaving the
// MARGIN_ROWS-wide window wasn't drawn last frame (the grid draws only the inner visible rows).
static void reconcileWindow(VideoGrid *grid)
{
   int start = (grid->scrollRow - MARGIN_ROWS) * GRID_COLS;
   int end   = (grid->scrollRow + grid->visibleRows + MARGIN_ROWS) * GRID_COLS;
   if (start < 0) start = 0;
   if (end > grid->results.count) end = grid->results.count;
   if (start == grid->winStart && end == grid->winEnd) return;

   int lo = start < grid->winStart ? start : grid->winStart;
   int hi = end   > grid->winEnd   ? end   : grid->winEnd;
   for (int i = lo; i < hi; i++) {
      int wasIn = (i >= grid->winStart && i < grid->winEnd);
      int nowIn = (i >= start && i < end);
      if      (nowIn && !wasIn) materializeTile(grid, i);
      else if (wasIn && !nowIn) dematerializeTile(grid, i);
   }
   grid->winStart = start;
   grid->winEnd   = end;
}

static void decodeWindowThumbnails(VideoGrid *grid)
{
   int done = 0;
   for (int i = grid->winStart; i < grid->winEnd && done < DECODES_PER_FRAME; i++)
      if (grid->thumbs[i].state == THUMB_FETCHED && grid->thumbs[i].jpeg && !grid->thumbs[i].tex.offset) {
         grid->thumbs[i].tex = loadGfxTextureMem(grid->thumbs[i].jpeg, (uint32_t)grid->thumbs[i].jpegLen);
         done++;
      }
}

static void moveSelection(VideoGrid *grid, int delta)
{
   int col = grid->selected % GRID_COLS;
   if (delta == 1  && col == GRID_COLS - 1) return;
   if (delta == -1 && col == 0)             return;
   int target = grid->selected + delta;
   if (target >= 0 && target < grid->results.count) grid->selected = target;
}

// ---- public interface ----

void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height)
{
   memset(grid, 0, sizeof *grid);
   grid->font = font;
   grid->x = x; grid->y = y; grid->width = width; grid->height = height;

   grid->tileW       = (width - (GRID_COLS - 1) * GRID_GAP) / GRID_COLS;
   grid->thumbH      = grid->tileW * 9 / 16;
   grid->rowStep     = grid->thumbH + TILE_TEXT_H + GRID_GAP;
   grid->visibleRows = height / grid->rowStep;
   if (grid->visibleRows < 1) grid->visibleRows = 1;

   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) {
      initLabel(&grid->rows[i],      font, 0, 0, grid->tileW, AUTO, ROW_TITLE, COLOR_SLATE_100, TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->metas[i],     font, 0, 0, grid->tileW, AUTO, META_SIZE, COLOR_SLATE_400, TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&grid->durations[i], font, 0, 0, AUTO,        AUTO, DUR_SIZE,  COLOR_SLATE_100, TEXT_NOWRAP,          "");
   }
}

void termVideoGrid(VideoGrid *grid)
{
   stopVideoGrid(grid);
   freeThumbnails(grid);
   for (int i = 0; i < MAX_SEARCH_RESULTS; i++) { freeLabel(&grid->rows[i]); freeLabel(&grid->metas[i]); freeLabel(&grid->durations[i]); }
}

void setGridWatchedPredicate(VideoGrid *grid, int (*isWatched)(const char *videoId)) { grid->isWatched = isWatched; }

// both source-changes are non-blocking: if a load is in flight, ask it to stop and defer the swap until the
// reap in updateVideoGrid, so mode / category switches never freeze the frame on an in-flight fetch.
static void changeSource(VideoGrid *grid, GridFetchFn fetch, void *user, const SearchResults *cached)
{
   grid->fetch = fetch;
   grid->fetchUser = user;
   grid->pendingCached = cached;
   if (!grid->threadActive) { applyDeferredSource(grid); return; }
   grid->stopWorker = 1;
   grid->reloadPending = 1;
   grid->stage = GRID_RUNNING;
}

void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, NULL); }
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user) { changeSource(grid, fetch, user, cached); }

void stopVideoGrid(VideoGrid *grid)
{
   if (grid->threadActive) { grid->stopWorker = 1; joinThread(grid->workerTid); grid->threadActive = 0; }
   grid->reloadPending = 0;
}

void updateVideoGrid(VideoGrid *grid)
{
   // reap a finished worker; a deferred source swap then resets and starts the new feed
   if (grid->workerDone && grid->threadActive) {
      joinThread(grid->workerTid); grid->threadActive = 0;
      if (grid->reloadPending) { grid->reloadPending = 0; applyDeferredSource(grid); }
   }
   if (grid->stage != GRID_READY) return;

   reconcileWindow(grid);
   decodeWindowThumbnails(grid);

   if      (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_RIGHT))) moveSelection(grid, 1);
   else if (isRepeatDue(&grid->horizontalRepeat, getPadButtonState(PAD_BTN_LEFT)))  moveSelection(grid, -1);
   if      (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_DOWN)))  moveSelection(grid, GRID_COLS);
   else if (isRepeatDue(&grid->verticalRepeat,   getPadButtonState(PAD_BTN_UP)))    moveSelection(grid, -GRID_COLS);

   int selRow = grid->selected / GRID_COLS;
   if (selRow < grid->scrollRow)                            grid->scrollRow = selRow;
   else if (selRow >= grid->scrollRow + grid->visibleRows)  grid->scrollRow = selRow - grid->visibleRows + 1;

   // idle background work: fetch thumbnails the window scrolled toward first, then the next page near the bottom
   if (!grid->threadActive && !grid->reloadPending) {
      if (grid->thumbFetched < thumbFrontier(grid)) {
         loadThumbnails(grid);
      } else if (grid->results.continuation[0] && grid->results.count < MAX_SEARCH_RESULTS) {
         int lastRow = (grid->results.count - 1) / GRID_COLS;
         if (selRow >= lastRow - 1) loadMore(grid);
      }
   }
}

static void drawTile(VideoGrid *grid, int i)
{
   int row = i / GRID_COLS, col = i % GRID_COLS;
   int tx = grid->x + col * (grid->tileW + GRID_GAP);
   int ty = grid->y + (row - grid->scrollRow) * grid->rowStep;
   const SearchResult *item = &grid->results.items[i];
   int watched = grid->isWatched && grid->isWatched(item->videoId);

   if (i == grid->selected)
      fillGfxRectangle(tx - 3, ty - 3, grid->tileW + 6, grid->thumbH + 6, COLOR_AMBER_300);

   if (grid->thumbs[i].tex.offset)
      drawGfxTexture(tx, ty, grid->tileW, grid->thumbH, grid->thumbs[i].tex, 0, 0, 1, 1, watched ? WATCHED_TINT : 0xFFFFFFFF, GFX_FILTER_LINEAR);
   else
      fillGfxRectangle(tx, ty, grid->tileW, grid->thumbH, PLACEHOLDER);

   if (item->duration[0] || item->isLive) {
      int badgeW = grid->durations[i].tt.tex.w + 12, badgeH = DUR_SIZE + 8;
      int badgeX = tx + grid->tileW - badgeW - 6, badgeY = ty + grid->thumbH - badgeH - 6;
      fillGfxRectangle(badgeX, badgeY, badgeW, badgeH, item->isLive ? BADGE_LIVE : BADGE_BG);
      drawLabelAt(&grid->durations[i], badgeX + 6, badgeY + 4);
   }

   int textY = ty + grid->thumbH + 6, alpha = watched ? WATCHED_ALPHA : 255;
   moveLabel(&grid->rows[i],  tx, textY);                   drawLabelAlpha(&grid->rows[i],  alpha);
   moveLabel(&grid->metas[i], tx, textY + ROW_TITLE + 8);   drawLabelAlpha(&grid->metas[i], alpha);
}

void drawVideoGrid(VideoGrid *grid)
{
   if (grid->stage != GRID_READY) return;
   for (int i = 0; i < grid->results.count; i++) {
      int row = i / GRID_COLS;
      if (row >= grid->scrollRow && row < grid->scrollRow + grid->visibleRows) drawTile(grid, i);
   }
}

GridStage            gridStage(const VideoGrid *grid)    { return grid->stage; }
int                  gridBusy(const VideoGrid *grid)     { return grid->threadActive && grid->job == JOB_MORE; }
int                  gridStopRequested(const VideoGrid *grid) { return grid->stopWorker; }
const SearchResults *gridResults(const VideoGrid *grid)  { return &grid->results; }
const SearchResult  *gridSelected(const VideoGrid *grid)
{
   return grid->results.count ? &grid->results.items[grid->selected] : NULL;
}
