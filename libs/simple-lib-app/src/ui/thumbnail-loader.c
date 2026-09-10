// thumbnail-loader - see thumbnail-loader.h.
//
// The workers only fetch bytes. Turning bytes into a texture touches the graphics chip, which is
// the drawing thread's alone, so that half happens there.

#include "ui/thumbnail-loader.h"

#include "dbg.h"
#include "http.h"
#include "printf.h"
#include "thread.h"

#include <stdlib.h>
#include <string.h>

#define TAG "[thumb] "

#define FETCH_MAX (512 * 1024)   // a tile-sized picture, with plenty of room to spare

static void fetchThread(uint64_t arg)
{
   ThumbnailLoader *loader = (ThumbnailLoader *)(uintptr_t)arg;

   uint8_t *buffer = (uint8_t *)malloc(FETCH_MAX);
   if (buffer) {
      for (;;) {
         int index = __sync_fetch_and_add(&loader->next, 1);
         if (index >= loader->count || loader->stopping) break;

         const char *url = loader->getUrl(index, loader->user);
         if (!url || !url[0]) { loader->items[index].state = THUMBNAIL_MISSING; continue; }

         int length = 0, status = 0;
         if (fetchHttp("GET", url, 0, 0, 0, 0, (char *)buffer, FETCH_MAX, &length, &status) != 0 ||
             status != 200 || length <= 0) {
            loader->items[index].state = THUMBNAIL_MISSING;
            continue;
         }

         uint8_t *kept = (uint8_t *)malloc(length);
         if (!kept) { loader->items[index].state = THUMBNAIL_MISSING; continue; }

         memcpy(kept, buffer, length);
         loader->items[index].bytes = kept;
         loader->items[index].byteCount = length;

         // the bytes must be visible to the drawing thread before the state says they are there
         __sync_synchronize();
         loader->items[index].state = THUMBNAIL_FETCHED;
      }
      free(buffer);
   }

   (void)__sync_fetch_and_add(&loader->running, -1);
   exitThread();
}

void startThumbnails(ThumbnailLoader *loader, int count, ThumbnailUrlFn getUrl, void *user)
{
   freeThumbnails(loader);

   memset(loader, 0, sizeof *loader);
   loader->count = count > THUMBNAILS_MAX ? THUMBNAILS_MAX : count;
   loader->getUrl = getUrl;
   loader->user = user;

   for (int worker = 0; worker < THUMBNAIL_THREADS; worker++) {
      sys_ppu_thread_t thread;
      if (spawnThread(&thread, fetchThread, (uint64_t)(uintptr_t)loader, THREAD_PRIORITY_LOW,
                      THREAD_STACK_SIZE_64KB, "thumbnails") == 0)
         (void)__sync_fetch_and_add(&loader->running, 1);
   }
}

void takeLoadedThumbnails(ThumbnailLoader *loader)
{
   for (int index = 0; index < loader->count; index++) {
      Thumbnail *item = &loader->items[index];
      if (item->state != THUMBNAIL_FETCHED) continue;

      item->texture = loadGfxTextureMem(item->bytes, (uint32_t)item->byteCount);
      free(item->bytes);
      item->bytes = 0;
      item->byteCount = 0;
      item->state = item->texture.w > 0 ? THUMBNAIL_READY : THUMBNAIL_MISSING;
   }
}

GfxTexture getThumbnail(const ThumbnailLoader *loader, int index)
{
   GfxTexture none = { 0 };
   if (index < 0 || index >= loader->count) return none;
   return loader->items[index].state == THUMBNAIL_READY ? loader->items[index].texture : none;
}

void freeThumbnails(ThumbnailLoader *loader)
{
   loader->stopping = 1;
   while (loader->running > 0) sleepMs(2);

   for (int index = 0; index < loader->count; index++) {
      Thumbnail *item = &loader->items[index];
      if (item->bytes) { free(item->bytes); item->bytes = 0; }
      if (item->state == THUMBNAIL_READY) freeGfxTexture(&item->texture);
      item->state = THUMBNAIL_WANTED;
   }
   loader->count = 0;
   loader->stopping = 0;
}
