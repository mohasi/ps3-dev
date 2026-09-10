#pragma once

// xcloud-media - turning the decrypted picture and sound into something to watch and hear.
//
// The two arrive interleaved on one socket and are told apart by the number the machine gave each
// in its description. Everything from there to the screen and the speakers lives here.

#include <stdint.h>

void openXcloudMedia(void);
void closeXcloudMedia(void);

// Takes one decrypted media packet, whichever it is. Returns 1 when that produced a new picture,
// which is the only moment worth drawing: putting a frame up waits for the display, and doing
// that on every packet leaves the socket unread long enough to lose the next one.
int feedXcloudMedia(const uint8_t *packet, int length);

// Puts the newest picture on screen. Does nothing until one has been decoded.
void drawXcloudMedia(void);

// For the line printed when the connection ends.
void reportXcloudMedia(void);
