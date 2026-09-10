// decode-opus - see decode-opus.h.
//
// Thin on purpose. The vendored decoder under opus/ is left exactly as upstream wrote it, and
// everything that knows about it lives here.

#include "decode-opus.h"

#include "dbg.h"

#include "opus.h"

#include <stdlib.h>

#define TAG "[opus] "

// opus works in whole numbers here, so its samples are already the shape the console wants
struct OpusDecoderHandle {
   OpusDecoder *decoder;
};

OpusDecoderHandle *createOpusDecoder(int rate, int channels)
{
   OpusDecoderHandle *handle = (OpusDecoderHandle *)calloc(1, sizeof *handle);
   if (!handle) return 0;

   int error = OPUS_OK;
   handle->decoder = opus_decoder_create(rate, channels, &error);
   if (!handle->decoder) {
      logError(TAG "could not start a decoder, error %d\n", error);
      free(handle);
      return 0;
   }

   logInfo(TAG "ready at %d Hz, %d channels\n", rate, channels);
   return handle;
}

int decodeOpus(OpusDecoderHandle *decoder, const uint8_t *packet, int length, int16_t *out, int capacity)
{
   if (!decoder) return -1;

   int samples = opus_decode(decoder->decoder, packet, length, out, capacity, 0);
   if (samples < 0) {
      logWarn(TAG "a packet would not decode, error %d\n", samples);
      return -1;
   }
   return samples;
}

void destroyOpusDecoder(OpusDecoderHandle *decoder)
{
   if (!decoder) return;

   opus_decoder_destroy(decoder->decoder);
   free(decoder);
}
