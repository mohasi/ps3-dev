#pragma once

#include <stdint.h>
#include "proc-mem.h"   // readProcMem/writeProcMem, the shared process-memory ABI (simple-lib-plugin)

// The game-specific side of process introspection: enumerating the running game's
// scannable segments. The read/write primitives it builds on live in proc-mem.h.

#ifdef __cplusplus
extern "C" {
#endif

// a scannable memory range in the game process (base + byte size).
typedef struct { uint32_t base; uint32_t size; } GameRange;

// collect the running game's scannable segments (every loaded module's loadable
// segments) into rangesOut, up to maxRanges; returns the count. 0 = enumeration
// unavailable on this CFW (the ps3mapi segments opcode is absent), so the caller
// falls back to a fixed address window.
int getGameScanRanges(uint32_t pid, GameRange *rangesOut, int maxRanges);

#ifdef __cplusplus
}
#endif
