#pragma once

// dynarray - the grow-by-doubling step for a realloc'd array. callers own the
// storage (pointer + element count + capacity) and the element type; this just
// ensures room for one more batch and reallocates when it runs out.

#include <stdlib.h>

// grows *items to hold at least `needed` elements of `elemSize`, doubling the
// capacity until it fits (seeding from 64 when empty). returns 1 on success
// (storage and *capacity updated), 0 on allocation failure — *items and
// *capacity are left untouched, so the caller's array stays valid.
static inline int growArrayRaw(void **items, int *capacity, int needed, size_t elemSize)
{
   if (needed <= *capacity) return 1;
   int cap = *capacity > 0 ? *capacity : 64;
   while (cap < needed) cap *= 2;
   void *grown = realloc(*items, (size_t)cap * elemSize);
   if (!grown) return 0;
   *items = grown;
   *capacity = cap;
   return 1;
}

#define growArray(items, capacity, needed) growArrayRaw((void **)(void *)&(items), (capacity), (needed), sizeof(*(items)))
