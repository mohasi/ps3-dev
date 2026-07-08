#pragma once

// video-grid - a reusable, pad-navigable grid of video results. Owns the whole display engine: a background
// worker that fetches result pages + thumbnail jpegs, windowed materialisation (only the on-screen band of
// tiles holds a decoded thumbnail texture + rasterised labels, so VRAM stays flat at any scroll depth),
// 2D D-pad navigation with hold-to-scroll, and infinite scroll. Screens supply a GridFetchFn (which encodes
// the mode: trending / search / channel + sort) and read back the selection; the grid knows nothing about
// modes. See search.c / home.c for callers.

#include "extractor.h"          // SearchResults / SearchResult
#include "font.h"               // Font
#include "ui/label.h"           // Label
#include "button-repeat.h"      // ButtonRepeat
#include "thread.h"             // sys_ppu_thread_t
#include <stdint.h>

#define GRID_COLS     4
#define GRID_THREADS  4         // parallel thumbnail fetch threads

typedef enum { GRID_RUNNING, GRID_READY, GRID_EMPTY } GridStage;

// what the background worker is doing this spawn. LOAD/CACHE fetch page 1 (CACHE restores kept results first);
// MORE appends the next continuation page; THUMBS extends thumbnails to the current window frontier.
typedef enum { JOB_LOAD, JOB_CACHE, JOB_MORE, JOB_THUMBS } GridJob;

// fetch one page into out: token NULL = first page, else a continuation / sort token. 0 ok, negative error.
typedef int (*GridFetchFn)(const char *token, SearchResults *out, void *user);

// one thumbnail: jpeg fetched by the worker and kept; the texture is decoded only while the tile is in the
// visible window (tex.offset != 0) and freed when it scrolls away, re-decoding from the kept jpeg on return.
typedef enum { THUMB_NONE, THUMB_FETCHED, THUMB_FAILED } ThumbState;
typedef struct {
   volatile ThumbState state;
   uint8_t   *jpeg;
   int        jpegLen;
   GfxTexture tex;
} GridThumb;

typedef struct {
   Font            *font;
   int              x, y, width, height;                   // grid rectangle on screen
   int              tileW, thumbH, rowStep, visibleRows;   // derived geometry

   SearchResults    results;
   GridThumb        thumbs[MAX_SEARCH_RESULTS];
   Label            rows[MAX_SEARCH_RESULTS];              // per-tile title
   Label            metas[MAX_SEARCH_RESULTS];             // per-tile "author - views - age"
   Label            durations[MAX_SEARCH_RESULTS];         // per-tile duration / LIVE badge

   int              selected, scrollRow, winStart, winEnd;
   ButtonRepeat     horizontalRepeat, verticalRepeat;

   // background worker + thumbnail fetch cursor
   volatile GridStage stage;
   volatile int     searchParsed, workerDone, stopWorker;
   GridJob          job;
   int              thumbFetched;                          // thumbnails fetched so far (contiguous from 0)
   int              threadActive, reloadPending;
   sys_ppu_thread_t workerTid;
   volatile int     thumbNext, thumbEnd;

   GridFetchFn      fetch;
   void            *fetchUser;
   const SearchResults *pendingCached;                     // set = next (re)load restores these instantly
   int              (*isWatched)(const char *videoId);     // fade predicate (NULL = no fade)
} VideoGrid;

void initVideoGrid(VideoGrid *grid, Font *font, int x, int y, int width, int height);
void termVideoGrid(VideoGrid *grid);

void setGridWatchedPredicate(VideoGrid *grid, int (*isWatched)(const char *videoId));

// load a fresh feed from page 1. non-blocking: if a load is already running it's swapped once that finishes,
// so mode/sort switches never freeze the frame.
void setGridSource(VideoGrid *grid, GridFetchFn fetch, void *user);
// restore already-known results instantly (grid then only re-fetches thumbnails); fetch/user drive paging.
void setGridCached(VideoGrid *grid, const SearchResults *cached, GridFetchFn fetch, void *user);

void updateVideoGrid(VideoGrid *grid);   // nav + scroll + window + decode + infinite scroll; call each frame
void drawVideoGrid(VideoGrid *grid);
void stopVideoGrid(VideoGrid *grid);     // block until the worker stops (before playback / teardown)

GridStage            gridStage(const VideoGrid *grid);
int                  gridBusy(const VideoGrid *grid);       // a fetch worker is running (results may still be growing)
int                  gridStopRequested(const VideoGrid *grid); // a source swap asked the worker to stop (bail long fetches)
const SearchResult  *gridSelected(const VideoGrid *grid);   // NULL if empty
const SearchResults *gridResults(const VideoGrid *grid);    // for caching + channel sort tokens
