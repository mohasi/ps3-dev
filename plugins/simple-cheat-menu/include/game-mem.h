#pragma once

#include <stdint.h>

// read from / write to a process's memory by pid, via ps3mapi. the single home for
// the process-memory ABI: the cheat scan/poke (the game's pid) and the export hook
// (vsh's own pid) both go through here. tries the debug syscalls (904/905) first, then
// falls back to cobra ps3mapi (syscall 8) and remembers which path the CFW allows.
// returns 0 on success, a non-zero rc otherwise. no logging — callers log as they need.

#ifdef __cplusplus
extern "C" {
#endif

int readProcMem(uint32_t pid, uint32_t address, void *out, uint32_t size);
int writeProcMem(uint32_t pid, uint32_t address, const void *src, uint32_t size);

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
