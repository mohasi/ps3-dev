#pragma once

// Detour a vsh export (by library name + fnid) so our own function runs on
// every call and the original then proceeds unchanged. Used to ride the paf
// per-frame callback for drawing, and later to detect the impose overlay.
//
// The patched export entry is a raw far-jump into an asm stub. On entry r2
// still holds the *callee's* toc, so the stub saves the inbound arg/toc/lr
// state, switches r2 to OUR module's toc (captured at install), calls the C
// hook, restores everything, then runs the original via a trampoline of the
// displaced instructions. This mirrors the TOC-independent trampoline idiom
// in module-hook.h and avoids the whole-plugin NoTocRestore build model the
// FPS-counter reference relies on.
//
// One hook at a time (single trampoline slot). The hook body must be light:
// it runs on vsh's frame thread every frame.

#include <stdint.h>

// install a detour. hookFn takes no args and must not disturb the call.
// returns 0 on success, negative on failure:
//   -1 export not found   -3 trampoline write failed   -4 entry patch failed
int installExportHook(const char *libName, uint32_t fnid, void (*hookFn)(void));
