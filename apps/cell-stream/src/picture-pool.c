#include "picture-pool.h"

#include "gfx.h"

void resetPicturePool(PicturePool *pool)
{
   pool->published = pool->previouslyPublished = -1;
   pool->drawn = pool->previouslyDrawn = -1;
}

int allocPicturePool(PicturePool *pool, size_t bytes)
{
   for (int which = 0; which < PICTURE_POOL_SIZE; which++) {
      pool->buffers[which] = allocGfxVideoBuffer(bytes);
      if (!pool->buffers[which]) { freePicturePool(pool); return -1; }
   }
   return 0;
}

int hasPicturePoolBuffers(const PicturePool *pool)
{
   for (int which = 0; which < PICTURE_POOL_SIZE; which++)
      if (pool->buffers[which]) return 1;
   return 0;
}

void freePicturePool(PicturePool *pool)
{
   for (int which = 0; which < PICTURE_POOL_SIZE; which++) {
      if (!pool->buffers[which]) continue;
      freeGfxVideoBuffer(pool->buffers[which]);
      pool->buffers[which] = 0;
   }
}

int getFreePicture(const PicturePool *pool)
{
   int which;
   for (which = 0; which < PICTURE_POOL_SIZE; which++)
      if (which != pool->published && which != pool->previouslyPublished
          && which != pool->drawn && which != pool->previouslyDrawn) break;

   return which < PICTURE_POOL_SIZE ? which : 0;   // five buffers against at most four exclusions
}
