# simple-lib-plugin

Header-only helpers that only make sense inside a VSH plugin (a `.sprx` loaded into `vsh.self`).
Everything that is not PRX-specific — logging, `printf`, files, threads, strings, the syscall layer,
the debug bridge — lives in `simple-lib-core` and is shared with apps. This library holds just the
three pieces that are tied to the VSH runtime.

## Working inside a VSH plugin: the constraints

These shape everything here, and every plugin that links this library:

- **Every symbol must resolve.** A PRX that references a symbol the loader can't resolve dies
  silently at load time — no crash, no log, it just never starts. Stick to what the shared libraries
  already wrap. Never call libc `printf` / `sprintf` / `snprintf`; use the `printf.h` wrappers in
  `simple-lib-core`.
- **`_start()` must not block.** It runs on the loader thread and must return promptly. No locks, no
  socket I/O, no waits on a thread that hasn't been spawned yet — do that work on your own worker
  thread.
- **Allocations stay small.** VSH-PRX runtime allocations must stay in the double-digit KB range;
  64 KB is the hard ceiling for a single allocation and you cannot stack several 64 KB pages
  back-to-back. Prefer grabbing memory on demand over always-reserved memory.

## What it provides

- **vsh.h** — the two VSH-only imports, reachable only when the PRX is loaded into `vsh.self`:
  `vshNotify(msg)` shows an XMB notification, and `isXmbReady()` reports whether the XMB has
  finished coming up. Each links its own stub archive (`libvshtask_export_stub.a`,
  `libvshmain_export_stub.a`) — only link the one whose symbol you use. Non-VSH code should not
  include this; use `syscall.h` in `simple-lib-core` for generic LV2 wrappers instead.
- **module-hook.h** — an outgoing-call trace engine for a loaded PRX. It swaps each import slot for a
  small trampoline that records every call (caller, target, first few arguments) into a ring buffer
  and then jumps to the real function, so the callee runs unchanged. Everything an armed session
  owns — the ring, the module table, the per-slot table — fits in a single 64 KB heap block to
  respect the allocation limit above; disarming frees the whole block.
- **module-inspect.h** — walkers that read a loaded image's linkage. Given a module's memory
  segments, they scan for its export (`.lib.ent`) and import (`.lib.stub`) records and emit one text
  row per record, with every pointer bounds-checked against the segment list first so a malformed
  record is skipped rather than faulted on. Callers pass in the row tag names, so the same walker
  serves both PRX module-info and process-level main-exe inspection.

Both `module-hook.h` and `module-inspect.h` build on `syscall.h` (from `simple-lib-core`) for the
prx-inspection and memory syscalls.

## Usage

1. Add `$(SolutionDir)libs\simple-lib-plugin\include` to the include directories, alongside
   `simple-lib-core\include`.
2. There is no archive to link — this library is header-only. Link `libsimple-lib-core.a` for the
   compiled pieces (printf, logging, files, syscalls, …).
3. Include headers as needed: `#include "vsh.h"`, `#include "module-hook.h"`, and for the shared
   pieces `#include "syscall.h"`, `#include "dbg.h"`, etc. from `simple-lib-core`.

## Layout

```
simple-lib-plugin/
├── simple-lib-plugin.vcxproj   # surfaces the headers in the solution; no .c sources, no .a output
└── include/
   ├── vsh.h
   ├── module-hook.h
   └── module-inspect.h
```

## Design

All three headers are `static inline`, so they compile straight into the consuming PRX. The library
has no `.c` sources and produces no `.a` — the vcxproj exists only to show the headers in the
solution and to mark "PRX-only context" as a project boundary.
