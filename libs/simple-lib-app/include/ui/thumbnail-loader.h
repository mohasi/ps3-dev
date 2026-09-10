#pragma once

// thumbnail-loader - fetching a screenful of small pictures at once.
//
// A grid of tiles wants a picture per tile, each a separate download. One at a time is far too
// slow over a connection with any distance in it, so several are fetched at once and turned into
// textures as they land.
//
// The fetching happens on worker threads; turning the bytes into a texture happens on the thread
// that draws, because that is the only one allowed to. Call takeLoadedThumbnails once a frame.

#include "gfx.h"

#include <stdint.h>

#define THUMBNAILS_MAX    512
#define THUMBNAIL_THREADS 4

typedef enum {
   THUMBNAIL_WANTED,    // not asked for yet
   THUMBNAIL_FETCHED,   // bytes are in, waiting to become a texture
   THUMBNAIL_READY,
   THUMBNAIL_MISSING    // no picture, or the fetch failed
} ThumbnailState;

typedef struct {
   volatile ThumbnailState state;
   uint8_t *bytes;
   int byteCount;
   GfxTexture texture;
} Thumbnail;

// Where each picture comes from, asked of the caller by position so the caller keeps its own list.
// Return an empty string for a tile that has no picture.
typedef const char *(*ThumbnailUrlFn)(int index, void *user);

typedef struct {
   Thumbnail items[THUMBNAILS_MAX];
   int count;
   ThumbnailUrlFn getUrl;
   void *user;
   volatile int next, stopping, running;
} ThumbnailLoader;

// Starts fetching count pictures. Returns straight away; the pictures appear over the next while.
void startThumbnails(ThumbnailLoader *loader, int count, ThumbnailUrlFn getUrl, void *user);

// Turns whatever has arrived into textures. Called once a frame from the drawing thread.
void takeLoadedThumbnails(ThumbnailLoader *loader);

// The texture for one tile, or a texture with no width when there is nothing to show yet.
GfxTexture getThumbnail(const ThumbnailLoader *loader, int index);

// Stops the workers and gives back everything. Blocks until the workers are out.
void freeThumbnails(ThumbnailLoader *loader);
