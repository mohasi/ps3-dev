#pragma once

// frame-arena - the buffers a picture is rebuilt in, before it goes to the decoder.
//
// Both video paths need the same thing: a handful of buffers used in turn, each big enough for the
// largest picture the stream can send, and each one left untouched for a while after it is handed
// over because the decoder reads a picture where it lies rather than copying it. They had one set
// each, 9.4 MB between them, and only one path can be running at a time, so there is one set here.
//
// Claiming it is not for safety, it is so that a mistake shows up in the log as a refused claim
// rather than as two paths quietly writing over each other.

#include <stdint.h>

// Six. The PC path also uses them as the queue between its receive thread and its decode thread,
// and both paths need a picture handed over to stay untouched well past the next one, because the
// decoder reads it where it lies. With two, over half the pictures came out of the decoder broken.
#define FRAME_ARENA_SLOTS      6
// One picture. A keyframe at a high bitrate is far larger than an ordinary picture; this has never
// been measured, so it is headroom rather than a size (the two paths previously guessed 1 MB and
// 512 KB for the same quantity).
#define FRAME_ARENA_SLOT_BYTES (1024 * 1024)

// Takes the arena for one video path. `owner` names it in the log. 0 when it was free, -1 when
// someone else already holds it, which means a path was left running.
int claimFrameArena(const char *owner);
void releaseFrameArena(void);

uint8_t *getFrameArenaSlot(int slot);
