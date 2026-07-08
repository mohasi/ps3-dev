// video-source - seekable, read-ahead reader over a local file or a streamed url. See video-source.h.
#include "video-source.h"
#include <string.h>
#include <stdlib.h>

#define SOURCE_BUFFER_SIZE  65536   // read-ahead window; container headers are read a few bytes at a time

// the two backend primitives the read-ahead layer sits on; dispatch on the source kind.
static int64_t readBackend(VideoSource *source, void *out, uint64_t length)
{
   return source->isHttp ? readHttpStream(source->http, out, length) : readFs(&source->file, out, length);
}

static int seekBackend(VideoSource *source, uint64_t offset)
{
   if (source->isHttp) return seekHttpStream(source->http, offset);
   return seekFs(&source->file, (int64_t)offset, VFS_SEEK_SET) < 0 ? -1 : 0;
}

int openVideoSource(VideoSource *source, const char *path)
{
   memset(source, 0, sizeof *source);

   source->buffer = (uint8_t *)malloc(SOURCE_BUFFER_SIZE);
   if (!source->buffer) return -1;

   if (isHttpUrl(path)) {
      source->http = openHttpStream(path);
      if (!source->http) { free(source->buffer); source->buffer = 0; return -1; }
      source->isHttp = 1;
      source->size   = getHttpStreamSize(source->http);
   } else {
      VfsStat stat;
      if (statPath(path, &stat) != 0 || stat.isDir) { free(source->buffer); source->buffer = 0; return -1; }
      if (openFs(path, VFS_O_RDONLY, &source->file) != 0) { free(source->buffer); source->buffer = 0; return -1; }
      source->size = stat.size;
   }
   source->isOpen = 1;
   return 0;
}

// makes sure the backend is positioned at `offset` before a raw read; tracks the position so we only
// issue a seek when the backend has actually drifted.
static int seekDescriptor(VideoSource *source, uint64_t offset)
{
   if (source->filePos == offset) return 0;
   if (seekBackend(source, offset) != 0) return -1;
   source->filePos = offset;
   return 0;
}

int64_t readVideoSource(VideoSource *source, void *buffer, uint64_t length)
{
   if (!source->isOpen) return -1;

   uint8_t *out = (uint8_t *)buffer;
   uint64_t served = 0;
   while (length > 0) {
      // serve from the read-ahead buffer whenever pos falls inside it
      if (source->pos >= source->bufferStart && source->pos < source->bufferStart + source->bufferLen) {
         uint64_t available = source->bufferStart + source->bufferLen - source->pos;
         uint64_t take = length < available ? length : available;
         memcpy(out, source->buffer + (source->pos - source->bufferStart), take);
         out += take; source->pos += take; length -= take; served += take;
         continue;
      }

      // a large read bypasses the buffer entirely
      if (length >= SOURCE_BUFFER_SIZE) {
         if (seekDescriptor(source, source->pos) != 0) break;
         int64_t got = readBackend(source, out, length);
         if (got <= 0) break;
         out += got; source->pos += got; source->filePos += got; length -= got; served += got;
         continue;
      }

      // otherwise refill the buffer at the current position
      if (seekDescriptor(source, source->pos) != 0) break;
      int64_t got = readBackend(source, source->buffer, SOURCE_BUFFER_SIZE);
      if (got <= 0) break;
      source->bufferStart = source->pos;
      source->bufferLen   = (int)got;
      source->filePos     = source->pos + got;
   }
   return (int64_t)served;
}

int seekVideoSource(VideoSource *source, uint64_t offset)
{
   if (!source->isOpen) return -1;
   source->pos = offset;   // the backend is only moved lazily on the next buffer miss
   return 0;
}

uint64_t getVideoSourcePosition(const VideoSource *source) { return source->pos; }
uint64_t getVideoSourceSize(const VideoSource *source) { return source->size; }

void closeVideoSource(VideoSource *source)
{
   if (source->isOpen) {
      if (source->isHttp) closeHttpStream(source->http);
      else                closeFs(&source->file);
      source->isOpen = 0;
   }
   free(source->buffer);
   source->buffer = 0;
}

int readVideoSourceAt(VideoSource *source, uint64_t offset, void *buffer, uint64_t length)
{
   if (seekVideoSource(source, offset) != 0) return -1;

   uint8_t *out = (uint8_t *)buffer;
   uint64_t remaining = length;
   while (remaining > 0) {
      int64_t got = readVideoSource(source, out, remaining);
      if (got <= 0) return -1;
      out       += got;
      remaining -= (uint64_t)got;
   }
   return 0;
}
