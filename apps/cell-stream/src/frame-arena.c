#include "frame-arena.h"

#include "dbg.h"

#define TAG "[cst] "

static uint8_t slots[FRAME_ARENA_SLOTS][FRAME_ARENA_SLOT_BYTES];
static const char *heldBy;

int claimFrameArena(const char *owner)
{
   if (heldBy) {
      logError(TAG "frame arena: %s wanted it while %s still holds it\n", owner, heldBy);
      return -1;
   }
   heldBy = owner;
   return 0;
}

void releaseFrameArena(void)
{
   heldBy = 0;
}

uint8_t *getFrameArenaSlot(int slot)
{
   return slots[slot];
}
