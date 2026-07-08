// mp4 - shared ISOBMFF box reader. See mp4.h.
#include "mp4.h"

void fourccToStr(uint32_t fourcc, char out[5])
{
   out[0] = (char)(fourcc >> 24); out[1] = (char)(fourcc >> 16);
   out[2] = (char)(fourcc >> 8);  out[3] = (char)fourcc; out[4] = 0;
}

int readMp4Box(VideoSource *source, uint64_t off, uint32_t *type, uint64_t *boxSize, uint64_t *headerLen)
{
   uint8_t head[16];
   if (readVideoSourceAt(source, off, head, 8) != 0) return -1;
   uint64_t size = readU32BE(head);
   *type = readU32BE(head + 4);
   if (size == 1) {
      if (readVideoSourceAt(source, off + 8, head + 8, 8) != 0) return -1;
      size = readU64BE(head + 8);
      *headerLen = 16;
   } else {
      // size 0 = "box extends to EOF" - only resolvable when the total size is known. on an unknown-size
      // stream (size()==0) leave it 0 so the `size < *headerLen` check below fails the box and callers
      // treat it as end-of-boxes, instead of underflowing 0-off into a bogus huge size.
      if (size == 0) { uint64_t total = getVideoSourceSize(source); size = total > off ? total - off : 0; }
      *headerLen = 8;
   }
   if (size < *headerLen) return -1;
   *boxSize = size;
   return 0;
}

int findMp4ChildBox(VideoSource *source, uint64_t start, uint64_t end, uint32_t wantType, uint64_t *payloadStart, uint64_t *payloadEnd)
{
   uint64_t pos = start;
   int guard = 0;
   while (pos + 8 <= end && guard++ < 256) {
      uint32_t type; uint64_t size, headerLen;
      if (readMp4Box(source, pos, &type, &size, &headerLen) != 0) return -1;
      if (pos + size > end || size == 0) return -1;
      if (type == wantType) { *payloadStart = pos + headerLen; *payloadEnd = pos + size; return 0; }
      pos += size;
   }
   return -1;
}
