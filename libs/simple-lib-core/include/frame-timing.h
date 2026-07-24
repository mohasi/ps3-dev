#pragma once

// frame-timing - a malloc-free frame-pacing tracker. feed it one timestamp per presented frame;
// read back the latest frame interval, a running fps, and the recent history for a graph. It holds
// no gfx and no libc and keeps a fixed ring, so both apps and vsh plugins can use it (link
// simple-lib-core). The caller supplies the clock in microseconds, so it needs none of its own.

#include <stdint.h>

#define FRAME_TIMING_HISTORY 256

typedef struct {
   uint32_t intervalUs[FRAME_TIMING_HISTORY];   // recent frame intervals; index 0..count-1 are valid
   int      head;                               // next write slot
   int      count;                              // valid entries (<= FRAME_TIMING_HISTORY)
   uint64_t lastUs;                             // previous frame's timestamp, 0 before the first
} FrameTiming;

static inline void resetFrameTiming(FrameTiming *timing)
{
   timing->head = 0;
   timing->count = 0;
   timing->lastUs = 0;
}

// call once per presented frame. the first call after a reset only seeds the clock (no interval yet).
static inline void noteFrame(FrameTiming *timing, uint64_t nowUs)
{
   if (timing->lastUs != 0 && nowUs > timing->lastUs) {
      uint64_t delta = nowUs - timing->lastUs;
      if (delta > 0xFFFFFFFF) delta = 0xFFFFFFFF;
      timing->intervalUs[timing->head] = (uint32_t)delta;
      timing->head = (timing->head + 1) % FRAME_TIMING_HISTORY;
      if (timing->count < FRAME_TIMING_HISTORY) timing->count++;
   }
   timing->lastUs = nowUs;
}

// most recent frame interval in microseconds (0 if none recorded yet)
static inline uint32_t getLastFrameUs(const FrameTiming *timing)
{
   if (timing->count == 0) return 0;
   int newest = (timing->head - 1 + FRAME_TIMING_HISTORY) % FRAME_TIMING_HISTORY;
   return timing->intervalUs[newest];
}

// running fps across the whole window (0 until there is data)
static inline int getFrameFps(const FrameTiming *timing)
{
   if (timing->count == 0) return 0;
   uint64_t sum = 0;
   for (int i = 0; i < timing->count; i++) sum += timing->intervalUs[i];
   return sum == 0 ? 0 : (int)((uint64_t)timing->count * 1000000 / sum);
}

// fills out[] with the last n frame intervals (microseconds), oldest first; returns how many written
static inline int getFrameHistory(const FrameTiming *timing, uint32_t *out, int n)
{
   int available = timing->count < n ? timing->count : n;
   for (int i = 0; i < available; i++) {
      int index = (timing->head - available + i + FRAME_TIMING_HISTORY) % FRAME_TIMING_HISTORY;
      out[i] = timing->intervalUs[index];
   }
   return available;
}
