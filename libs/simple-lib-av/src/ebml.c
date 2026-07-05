// ebml - shared Matroska/EBML element reader. See ebml.h.
#include "ebml.h"

int readEbmlVint(VideoSource *source, uint64_t *value, int keepMarker, int *unknown)
{
   uint8_t first;
   if (readVideoSource(source, &first, 1) != 1) return 0;

   int length = 1;
   uint8_t mask = 0x80;
   while (length <= 8 && !(first & mask)) { length++; mask >>= 1; }
   if (length > 8) return 0;

   uint64_t result = first;
   for (int i = 1; i < length; i++) {
      uint8_t next;
      if (readVideoSource(source, &next, 1) != 1) return 0;
      result = (result << 8) | next;
   }

   if (!keepMarker) {
      uint64_t markerBit = (uint64_t)mask << (8 * (length - 1));
      uint64_t dataMask  = markerBit - 1;
      if (unknown) *unknown = ((result & dataMask) == dataMask);
      result &= dataMask;
   } else if (unknown) {
      *unknown = 0;
   }

   *value = result;
   return length;
}

int readEbmlElement(VideoSource *source, uint32_t *id, uint64_t *size, int *unknownSize)
{
   uint64_t rawId, rawSize;
   if (readEbmlVint(source, &rawId, 1, 0) == 0) return -1;
   if (readEbmlVint(source, &rawSize, 0, unknownSize) == 0) return -1;
   *id   = (uint32_t)rawId;
   *size = rawSize;
   return 0;
}
