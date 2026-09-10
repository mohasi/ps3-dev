#pragma once

// rtp-h264 - putting each picture back together from the pieces it arrives in.
//
// A single picture is far larger than one datagram, so it is cut up before sending and has to be
// rejoined here. The decoder will only accept whole pictures, and a picture missing a piece is
// worse than no picture at all, so an incomplete one is thrown away rather than passed on.

#include <stdint.h>

void openRtpH264(void);

// gives the frame arena back, so the other video path can have it
void closeRtpH264(void);

// Takes one decrypted video packet. When it completes a picture, points frame at it and returns
// its length; otherwise returns 0. Returns -1 when a piece went missing and the picture being
// built had to be abandoned.
int feedRtpH264(const uint8_t *packet, int length, const uint8_t **frame);

// How many pictures were abandoned because a piece never arrived.
int getRtpH264Dropped(void);
