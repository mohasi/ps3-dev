#pragma once

// timer - polling interval timer with callback

#include <stdint.h>

typedef void (*TimerCallback)(void *ctx);

typedef struct {
   uint64_t intervalUs;
   uint64_t lastFireUs;
   TimerCallback callback;
   void *ctx;
} Timer;

void initTimer(Timer *t, uint64_t intervalUs, TimerCallback callback, void *ctx);
void updateTimer(Timer *t);
