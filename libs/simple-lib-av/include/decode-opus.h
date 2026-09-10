#pragma once

// decode-opus - turns the sound a game stream sends into samples the console can play.
//
// Opus is what every WebRTC connection carries and the console has no decoder for it, so one is
// vendored here. It stays behind these four functions: nothing outside this file needs to know
// which decoder is underneath.

#include <stdint.h>

typedef struct OpusDecoderHandle OpusDecoderHandle;

// One decoder for one stream. rate is samples a second, channels is 1 or 2. NULL on failure.
OpusDecoderHandle *createOpusDecoder(int rate, int channels);

// Turns one packet into samples, written to out as pairs of whole numbers, one per channel.
// capacity is how many samples per channel out can hold; 960 covers the longest packet at 48 kHz.
// Returns how many samples per channel came out, or -1.
int decodeOpus(OpusDecoderHandle *decoder, const uint8_t *packet, int length, int16_t *out, int capacity);

void destroyOpusDecoder(OpusDecoderHandle *decoder);
