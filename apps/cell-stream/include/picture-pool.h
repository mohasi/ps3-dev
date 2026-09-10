#pragma once

// picture-pool - the decoded pictures both video paths hand to the graphics chip.
//
// Drawing a picture does not copy it: the chip reads it where it lies, during and after the call
// that draws it, so a buffer stays in use for a while after it was drawn. Decoding into the
// published, previously published, or two most recently drawn buffers paints over something still
// on screen (it showed as tearing), which is why there are five: one is always free.
//
// The pool has no lock of its own. Both callers already hold a lock covering the pool and their
// own bookkeeping together, and a second lock inside would only let the two disagree.

#include <stdint.h>
#include <stddef.h>

#define PICTURE_POOL_SIZE 5

typedef struct {
   uint8_t *buffers[PICTURE_POOL_SIZE];
   int published, previouslyPublished;
   int drawn, previouslyDrawn;
} PicturePool;

// forgets which picture is which, without touching the buffers. call before a stream starts.
void resetPicturePool(PicturePool *pool);

// takes all five buffers of the given size, or none of them. 0 on success.
int allocPicturePool(PicturePool *pool, size_t bytes);

int hasPicturePoolBuffers(const PicturePool *pool);
void freePicturePool(PicturePool *pool);

// the first buffer that is not being shown, was not just shown, and is not about to be
int getFreePicture(const PicturePool *pool);
