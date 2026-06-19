# simple-lib-core

Static library of context-neutral primitives shared by both
`simple-lib-plugin` (VSH PRX context) and `simple-lib-app` (app
context). Built with `-fno-builtin-printf -nodefaultlibs` so it is safe
to link into `vsh.self` PRXs; apps link it the same way.

## What it provides

- **printf** — drop-in `snprintf` / `vsnprintf` / `sprintf` that does
  not depend on libc dynamic imports. Header `printf.h`, compiled unit
  `src/printf.c`.
- **dbg** — leveled, timestamped logging via a single atomic
  `cellFsWrite` to `/dev_hdd0/tmp/dbg.txt`: `logInfo` / `logWarn` /
  `logError`. Optional sink (`setLogSink`) forwards each fully-formatted
  line after the disk write. Defines `BacklogLine` / `LOG_LINE_MAX`
  used by every pre-connect log ring.
- **syscall** — the whole LV2 syscall layer (moved here from
  simple-lib-plugin so apps and plugins share one copy). Generic inline-asm
  trampolines (`scCall1..scCall6`, `r11`=number / `r3..` args / `r3`
  result); the filesystem-device syscalls `mountDevBlind` (837, cobra
  /dev_blind mount) and `syncDevice` (839, `sys_fs_sync` — flush a volume's
  data + metadata + free-block bitmap to disk); and the prx/power/memory
  wrappers `sysPower` (379), `getPrxModuleIdByAddress` (461),
  `prxFinalizeSelf` (482), `prxList`/`prxName`/`prxInfo`/`prxLinkage`
  (494/495), `sysMemAllocate`/`sysMemFree` (348/349). prx-safe,
  header-only. (VSH-only NID stubs stay in `simple-lib-plugin/vsh.h`.)
- **file** — filesystem helpers (`readFile`, `writeFile`, `fileExists`,
  `makeDir`, `deleteFile`, `deleteTree`, `copyFile`, `copyTree`,
  `moveTree`) routed through the **vfs** layer below, so they work on the HDD,
  FAT32 USB (cellFs) and exFAT volumes alike — plus libc-free path utilities
  (`joinPath`, `toParentPath`,
  `getParentPath`, `getExtension`, `getBaseName`, `deviceRootOf`, `isValidFileName`,
  `isDir`, `formatSize`, `MAX_PATH_LEN`). The inline helpers are
  header-only; the cancellable, byte-reporting tree operations live in
  `src/file.c`: `measureTree`, `copyTreeProgress`, `deleteTreeProgress`,
  `mergeTreeProgress` (merge into an existing tree, replacing or keeping
  colliding file leaves), and `countTreeConflicts` (how many files a merge
  would land on, capped for a quick none/one/many check). Each takes
  `onBytes` / `cancelled` callbacks (either may be NULL). Copy/delete/move
  each issue one `syncDevice` at the batch boundary for crash-durability
  and accurate free-space reporting.
- **vfs** — the one filesystem abstraction every consumer goes through.
  A single `/`-rooted namespace where cellFs devices (`/dev_hdd0`, kernel-mounted
  FAT32 `/dev_usb000`) and virtual exFAT mounts (`/exfat0` …) sit side by side;
  `resolvePath` routes each call to the right backend (unmatched paths pass
  through to cellFs unchanged). Ops: `statPath`, `openDir`/`readDir`/`closeDir`,
  `openFs`/`readFs`/`writeFs`/`seekFs`/`fsyncFs`, `renamePath`, `makeDirPath`,
  `removeFilePath`/`removeDirPath`, `getFreeSpace`, `listMounts`, `getScheme`.
  The VFS owns USB hotplug detection (device presence is format-agnostic) and
  offers each newly-present device to registered backends via
  `registerVfsBackend(probe, release, shutdown)` — cellFs is built in, exFAT (and
  later NTFS) register at runtime. Lifecycle (`initVfs` / `pollMounts` /
  `shutdownVfs`) is driven by the host: the app's main loop, or a plugin's own
  thread — never on a request-serving path.
- **exfat** — hand-written, libc-free exFAT reader/writer for removable USB
  volumes via the LV2 storage manager: mount (superfloppy, MBR- and GPT-
  partitioned), directory listing, stat, free space, and read / write / create /
  delete / rename with real (Cell RTC) timestamps. Registers as a VFS backend
  via `initExfat()`, so callers reach exFAT through the same VFS API as the HDD.
  No FAT12/16/32 and no formatting. Cross-checked against ChaN's FatFs and
  validated with exfatprogs `fsck`.
- **usb-storage** — header-only USB mass-storage device layer (`getUsbDeviceId`,
  `isUsbDevicePresent`, `getStorageInfo`) shared by the VFS hotplug detection and
  the exFAT backend, so device identity/presence lives in one place.
- **thread** — `spawnThread()` PPU-thread spawn helper and stack-size
  constants.
- **ftp** — shared anonymous FTP server, managed as a singleton
  (`startFtpServer` → `FtpResult`, `stopFtpServer`, `isFtpServerRunning`,
  `isFtpPortAvailable`). The
  server is reusable from both apps and PRXs, keeps `/dev_blind` mount as a
  best-effort startup step, and leaves caller-specific boot policy (for example
  XMB readiness waits) outside the core module.
- **network** — `getLocalIpv4()` resolves the console's primary IPv4 address
  (the FTP listener binds to all interfaces, so the address is resolved here on
  demand rather than owned by the server).
- **string-utilities** — bounded copy / uppercase, length, case-
  insensitive compare, URL encode/decode, XML escaping, byte search,
  integer formatting.
- **wire** — framed TCP helpers used by both bridge server and producer
  client: `sendBytes`, `sendFrameHeader`, `sendFrame(fd, verb, payload,
  len)` (formats `<verb> <len>\n<bytes>`), `receiveLine` (newline-
  terminated command read).
- **log-backlog** — typed pre-connect line ring: `LogBacklog`,
  `pushLogBacklog` (overwrites oldest on overflow), `drainLogBacklog`
  (replays in chronological order via a callback). Reused by the bridge
  server (host pre-connect) and the producer client (bridge pre-connect).
- **bridge-client** — producer-side `registerWithBridge("plugin"|"app",
  "<name>")`. Installs the log sink synchronously, then a background
  thread connects to the bridge on `localhost:8785`, sends
  `REGISTER <kind> <name>\n`, and drains the local backlog so
  early-startup lines still reach the host.

## Layout

```
simple-lib-core/
├── simple-lib-core.vcxproj
├── include/
│   ├── printf.h
│   ├── dbg.h
│   ├── file.h
│   ├── vfs.h            # filesystem abstraction (router + backend contract)
│   ├── exfat.h          # exFAT reader/writer (VFS backend)
│   ├── usb-storage.h    # shared USB device id / presence / info
│   ├── ftp.h
│   ├── thread.h
│   ├── string-utilities.h
│   ├── wire.h
│   ├── log-backlog.h
│   └── bridge-client.h
└── src/
   ├── ftp.c             # shared FTP server implementation
   ├── vfs.c             # path-scheme router + cellFs/root backends + hotplug
   ├── exfat.c           # hand-written exFAT reader/writer backend
   ├── printf.c
   └── file.c            # cancellable tree ops (measure/copy/delete/merge/count)
```

## Usage

1. Build `simple-lib-core` first — produces `libsimple-lib-core.a` in
   `bin/Release/`.
2. In your plugin / app vcxproj:
   - Add `$(SolutionDir)libs\simple-lib-core\include` to include
    directories.
   - Place the `.a` **before** SDK stubs in `AdditionalDependencies` —
    our printf symbols must resolve before `libc_stub.a` or the PRX
    silently fails to load in VSH context.
3. Include headers as needed: `#include "dbg.h"`,
   `#include "bridge-client.h"`, etc.

## Design

Most utilities (`dbg.h`, `thread.h`, `string-utilities.h`, `wire.h`,
`log-backlog.h`, `bridge-client.h`, `usb-storage.h`, and the lighter half of
`file.h`) are `static inline`. The compiled units are `printf.c`, `file.c`,
`ftp.c`, `vfs.c`, and `exfat.c` (`file.c` holds the recursive tree operations;
`vfs.c` the filesystem router + cellFs backend + USB hotplug; `exfat.c` the
exFAT backend; `ftp.c` the shared FTP server). The library has no dependencies on
`simple-lib-plugin` or `simple-lib-app` — those depend on it, not the other way around.
