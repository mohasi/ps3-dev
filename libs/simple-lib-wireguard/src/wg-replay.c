#include "wg-replay.h"

#include "string-utilities.h"

void resetWgReplayWindow(WgReplayWindow *window)
{
   memSet(window, 0, sizeof *window);
}

int acceptWgCounter(WgReplayWindow *window, uint64_t counter)
{
   // the first packet of a session sets the window rather than being compared against it
   if (!window->hasReceived) {
      window->hasReceived = 1;
      window->highestCounter = counter;
      window->seenBefore = 1;
      return 1;
   }

   // ahead of everything seen so far: slide the window up, keeping whichever old bits still fit
   if (counter > window->highestCounter) {
      uint64_t advance = counter - window->highestCounter;
      window->seenBefore = advance >= WG_REPLAY_WINDOW ? 0 : (window->seenBefore << advance);
      window->seenBefore |= 1;
      window->highestCounter = counter;
      return 1;
   }

   uint64_t age = window->highestCounter - counter;
   if (age >= WG_REPLAY_WINDOW) return 0;      // older than we can remember, so it cannot be trusted

   uint64_t mark = (uint64_t)1 << age;
   if (window->seenBefore & mark) return 0;    // already arrived

   window->seenBefore |= mark;
   return 1;
}
