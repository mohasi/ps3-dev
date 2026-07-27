#pragma once

#include <stdint.h>

// Read from / write to a process's memory by pid. The single home for the process-memory
// ABI: a cheat scan/poke (the game's pid) and an export hook (vsh's own pid) both go through
// here. Tries the debug syscalls (904/905) first, then falls back to cobra ps3mapi (syscall 8)
// and remembers which path the CFW allows. Returns 0 on success, a non-zero rc otherwise. No
// logging, callers log as they need.

#ifdef __cplusplus
extern "C" {
#endif

int readProcMem(uint32_t pid, uint32_t address, void *out, uint32_t size);
int writeProcMem(uint32_t pid, uint32_t address, const void *src, uint32_t size);

#ifdef __cplusplus
}
#endif
