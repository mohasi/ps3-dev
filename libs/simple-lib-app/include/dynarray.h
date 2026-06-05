#pragma once

// dynarray - the grow-by-doubling step for a realloc'd array. callers own the
// storage (pointer + element count + capacity) and the element type; this just
// ensures room for one more batch and reallocates when it runs out.

#include <stdlib.h>

// grows `items` to hold at least `needed` elements of `elemSize`, doubling the
// capacity until it fits (seeding from 64 when empty). updates *capacity on
// success and returns the resulting storage pointer, or NULL on allocation
// failure (original storage remains valid).
static inline void *growArrayRaw(void *items, int *capacity, int needed, size_t elemSize)
{
    if (needed <= *capacity) return items;
    int cap = *capacity > 0 ? *capacity : 64;
    while (cap < needed) cap *= 2;
    void *grown = realloc(items, (size_t)cap * elemSize);
    if (!grown) return NULL;
    *capacity = cap;
    return grown;
}

#define growArray(items, capacity, needed) ((items) = growArrayRaw((items), (capacity), (needed), sizeof(*(items))))
