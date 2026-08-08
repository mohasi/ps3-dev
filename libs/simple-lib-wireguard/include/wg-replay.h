#pragma once

// Replay protection for received packets.
//
// Every packet carries a counter that only ever goes up. An attacker who records a packet could
// otherwise send it again later and the tunnel would accept it a second time. This remembers the
// highest counter seen and, in a 64 bit map, which of the previous 64 counters have already
// arrived, so a repeat is refused while packets that merely arrive out of order still get through.

#include <stdint.h>

#define WG_REPLAY_WINDOW 64

typedef struct {
   uint64_t highestCounter;
   uint64_t seenBefore;   // bit n set means the counter n places below the highest has arrived
   int      hasReceived;
} WgReplayWindow;

void resetWgReplayWindow(WgReplayWindow *window);

// 1 when the counter is new and the packet may be used, 0 when it repeats one already seen or is
// too old to judge.
int acceptWgCounter(WgReplayWindow *window, uint64_t counter);
