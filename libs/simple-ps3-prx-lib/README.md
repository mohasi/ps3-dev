# simple-ps3-prx-lib

Static library for PS3 VSH plugins (PRX context). Compiled with `-fno-builtin-printf -nodefaultlibs` for safe use inside `vsh.self`.

## What it provides

- **printf** — drop-in `snprintf`/`vsnprintf`/`sprintf` that doesn't depend on libc dynamic imports
- **dbg** — leveled, timestamped logging via single atomic `cellFsWrite`: `logInfo` / `logWarn` / `logError`. Optional sink (`setLogSink`) forwards each fully-formatted line after the disk write. Defines the shared `BacklogLine` / `LOG_LINE_MAX` used by every pre-connect log ring in this codebase.
- **bridge** — producer-side `registerWithBridge("plugin"|"app", "<name>")`. Installs the log sink synchronously, then spawns a background thread that connects to the bridge on `localhost:8785`, sends `REGISTER <kind> <name>\n`, and drains the local pre-connect backlog so early-startup lines still reach the host.
- **thread** — `spawnThread()` helper + stack-size constants for PPU threads.
- **file** — cellFs helpers (readFile, writeFile, fileExists, makeDir, deleteFile)
- **string-utilities** — case-insensitive compare, URL encode/decode, XML escaping, byte search, int formatting
- **syscall** — generic LV2 inline-asm trampolines (`scCall1..scCall5`) and reusable wrappers: `mountDevBlind` (837), `sysPower` (379), `prxGetModuleIdByAddress` (461), `prxFinalizeSelf` (482), `prxList` (494), `prxName` (495). Cross-checked against [psdevwiki LV2 syscalls](https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls) and RPCS3 sys_prx struct layouts.
- **vsh** — VSH-only NID stub exports: XMB readiness check (`isXmbReady`), VSH notification (`vshNotify`). Only resolvable inside `vsh.self`; non-VSH code should use `syscall.h` instead.

## Usage

1. Build `simple-ps3-prx-lib` first — produces `libsimple-ps3-prx-lib.a` in `bin/Release/`.
2. In your plugin's vcxproj:
   - Add `$(SolutionDir)libs\simple-ps3-prx-lib\include` to include directories.
   - Place the `.a` **before** SDK stubs in `AdditionalDependencies` (link order matters — our printf symbols must resolve before `libc_stub.a`).
3. Include headers as needed: `#include "dbg.h"`, `#include "file.h"`, etc.

## Layout

```
simple-ps3-prx-lib/
├── simple-ps3-prx-lib.vcxproj
├── include/            # public headers
│   ├── printf.h
│   ├── dbg.h
│   ├── bridge.h
│   ├── thread.h
│   ├── file.h
│   ├── string-utilities.h
│   ├── syscall.h
│   └── vsh.h
└── src/                # implementation
    └── printf.c        # Patrick Powell / Holger Weiss / Hector Martin port
```

## Link order

Our `.a` must appear before `libc_stub.a` in the linker inputs. Both are archives; the linker resolves symbols left-to-right. If `libc_stub.a` comes first, its printf stubs win — those reference dynamic imports that don't exist in VSH context, causing silent PRX load failure.

## Design

This lib is fully independent of `simple-ps3-lib`. Header-only utilities (`dbg.h`, `file.h`, `string-utilities.h`, `vsh.h`) are `static inline`. The only compiled unit is `printf.c`. Both libs contain their own copy of `file.h` since it is context-neutral.
