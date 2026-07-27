#pragma once

// Detour an export (by library name + fnid) so our own function runs on every
// call and the original then proceeds unchanged. The patched entry is a single
// atomic far-jump into an asm stub that saves the inbound arg/toc/lr state,
// switches r2 to OUR module's toc (captured at install), calls the C hook,
// restores everything, then runs the original via a trampoline of the displaced
// instructions.
//
// The hook is typed void(void) but the stub leaves r3/r4/r5/f1/f2 untouched
// before calling it, so a hook may declare the real callee signature and read
// its arguments directly.
//
// One hook at a time (single trampoline slot). The hook body must be light.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// returns 0 on success, negative on failure:
//   -1 export not found   -3 trampoline write failed   -4 entry patch failed
int installExportHook(const char *libName, uint32_t fnid, void (*hookFn)(void));

// same detour, but on a raw code address (an on-demand module resolved at
// runtime). returns 0 / -3 trampoline write failed / -4 entry patch failed.
int installCodeHook(uint32_t entry, void (*hookFn)(void));

// restore the patched entry. call before the module unloads so the export stops
// branching into memory the loader is about to reclaim. safe to call unhooked.
void uninstallCodeHook(void);

#ifdef __cplusplus
}
#endif
