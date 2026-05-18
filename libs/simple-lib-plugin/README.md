# simple-lib-plugin

Header-only static library for PS3 VSH plugins (PRX context). Holds
only the pieces that are **PRX-context-specific** � the cross-context
primitives (printf, dbg, file, thread, string utilities, wire framing,
bridge client) live in `simple-lib-core` and are consumed by both this
lib and `simple-lib-app`.

## What it provides

- **syscall** � generic LV2 inline-asm trampolines (`scCall1..scCall5`)
  and reusable wrappers: `mountDevBlind` (837), `sysPower` (379),
  `prxGetModuleIdByAddress` (461), `prxFinalizeSelf` (482),
  `prxList` (494), `prxName` (495). Cross-checked against
  [psdevwiki LV2 syscalls](https://www.psdevwiki.com/ps3/LV2_Functions_and_Syscalls)
  and RPCS3 sys_prx struct layouts.
- **vsh** � VSH-only NID stub exports: XMB readiness check
  (`isXmbReady`), VSH notification (`vshNotify`). Only resolvable inside
  `vsh.self`; non-VSH code should use `syscall.h` instead.

For logging (`dbg.h`), formatted I/O (`printf.h`), threads
(`thread.h`), file I/O (`file.h`), string utilities, the framed
wire protocol (`wire.h`), the pre-connect log ring (`log-backlog.h`),
or the producer-side bridge client (`bridge-client.h`), include those
headers from `simple-lib-core` directly.

## Usage

1. Add `$(SolutionDir)libs\simple-lib-plugin\include` to include
   directories (alongside `simple-lib-core\include`).
2. No archive to link � this lib is header-only. The single compiled
   unit (`printf.c`) lives in `simple-lib-core`; link
   `libsimple-lib-core.a` for that.
3. Include headers as needed: `#include "syscall.h"`, `#include "vsh.h"`.

## Layout

```
simple-lib-plugin/
??? simple-lib-plugin.vcxproj
??? include/
    ??? syscall.h
    ??? vsh.h
```

## Design

Both headers are `static inline` so consumers compile them straight
into their own PRX. This lib has no `.c` sources and therefore no
`.a` output � the vcxproj exists only to surface the headers in the
solution and to express "PRX-only context" as a project boundary.
