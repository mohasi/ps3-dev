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
  FAT32 USB (cellFs), exFAT and NTFS volumes alike — plus libc-free path utilities
  (`joinPath`, `toParentPath`,
  `getParentPath`, `getExtension`, `getBaseName`, `deviceRootOf`, `isValidFileName`,
  `isDir`, `formatSize`, `MAX_PATH_LEN`). The inline helpers are
  header-only; the cancellable, byte-reporting tree operations live in
  `src/vfs.c`: `measureTree`, `copyTreeProgress`, `deleteTreeProgress`,
  `mergeTreeProgress` (merge into an existing tree, replacing or keeping
  colliding file leaves), and `countTreeConflicts` (how many files a merge
  would land on, capped for a quick none/one/many check). Each takes
  `onBytes` / `cancelled` callbacks (either may be NULL). Copy/delete/move
  each issue one `syncDevice` at the batch boundary for crash-durability
  and accurate free-space reporting.
- **vfs** — the one filesystem abstraction every consumer goes through.
  A single `/`-rooted namespace where cellFs devices (`/dev_hdd0`, kernel-mounted
  FAT32 `/dev_usb000`) and virtual exFAT / NTFS mounts (`/exfat0` …, `/ntfs0` …) sit side by side;
  `resolvePath` routes each call to the right backend (unmatched paths pass
  through to cellFs unchanged). Ops: `statPath`, `openDir`/`readDir`/`closeDir`,
  `openFs`/`readFs`/`writeFs`/`seekFs`/`fsyncFs`, `renamePath`, `makeDirPath`,
  `removeFilePath`/`removeDirPath`, `getFreeSpace`, `listMounts`, `getScheme`.
  The VFS owns USB hotplug detection (device presence is format-agnostic) and
  offers each newly-present device to registered backends via
  `registerVfsBackend(probe, release, shutdown)` — cellFs is built in, exFAT and
  NTFS register at runtime. The router (`vfs.c`) names no concrete backend; the
  bringup that does (`initVfs` / `shutdownVfs` + the hotplug poll thread) lives in
  a separate TU, `vfs-init.c`, so a consumer that only routes paths links `vfs.c`
  alone and never pulls the exFAT/NTFS drivers (this linker can't strip
  unreachable code). `initVfs` is driven by the host: the app's main loop, or a
  plugin's own thread — never on a request-serving path.
- **exfat** — hand-written, libc-free exFAT reader/writer for removable USB
  volumes via the LV2 storage manager: mount (superfloppy, MBR- and GPT-
  partitioned), directory listing, stat, free space, and read / write / create /
  delete / rename with real (Cell RTC) timestamps. Registers as a VFS backend
  via `initExfat()`, so callers reach exFAT through the same VFS API as the HDD.
  No FAT12/16/32 and no formatting. Cross-checked against ChaN's FatFs and
  validated with exfatprogs `fsck`.
- **ntfs** — hand-written, libc-free NTFS reader/writer for removable USB volumes
  via the LV2 storage manager: mount (MBR- and GPT-partitioned), directory
  listing, stat, free space, and read / write / create / delete / rename.
  Registers as a VFS backend via `initNtfs()` (probed after exFAT, claims only
  `NTFS`-labelled volumes), so callers reach NTFS through the same VFS API as the
  HDD. No formatting. Validated against libntfs-3g on a raw image.
- **usb-storage** — header-only USB mass-storage device layer (`getUsbDeviceId`,
  `isUsbDevicePresent`, `getStorageInfo`) shared by the VFS hotplug detection and
  the exFAT/NTFS backends, so device identity/presence lives in one place.
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
- **http** — a transport-agnostic HTTP(S) client. One API — `fetchHttp` / `getHttp`
  for a one-shot request/response, `openHttpStream` / `readHttpStream` / `seekHttpStream`
  for seekable, never-fully-downloaded media streaming — over a pluggable transport bound
  once at startup. `initSystemHttp()` uses the console's firmware TLS (cellHttp: free weight,
  reaches RSA hosts; `transport-cellhttp.c` + firmware bringup in `cellhttp-stack.c`);
  `initModernHttp()` (in **simple-lib-https**) uses BearSSL and *overrides* it to also reach
  ECDSA-only hosts. Both backends pool and reuse keep-alive connections; `shutdownHttp()` drops
  the idle pool at exit. The light half (transport binding + one-shot, `http.c`) is malloc-free;
  the streaming engine (4 MB ring + prefetch thread, `http-stream.c`) is a separate TU pulled
  only by a caller that references `openHttpStream`, so a plugin that only calls `isHttpUrl` /
  `fetchHttp` never drags the heap into a VSH PRX.
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
│   ├── vfs-internal.h   # registry-lock primitives shared by vfs.c / vfs-init.c
│   ├── exfat.h          # exFAT reader/writer (VFS backend)
│   ├── ntfs.h           # NTFS reader/writer (VFS backend)
│   ├── usb-storage.h    # shared USB device id / presence / info
│   ├── ftp.h
│   ├── thread.h
│   ├── string-utilities.h
│   ├── wire.h
│   ├── log-backlog.h
│   ├── bridge-client.h
│   ├── http.h           # transport-agnostic HTTP(S) client (public API)
│   ├── http-transport.h # the pluggable transport vtable
│   ├── http-internal.h  # glue shared by http.c / http-stream.c (not public)
│   └── cellhttp-stack.h # firmware http/ssl/https bringup (cellHttp backend)
└── src/
   ├── ftp.c             # shared FTP server implementation
   ├── vfs.c             # path-scheme router + mount registry (names no backend)
   ├── vfs-init.c        # VFS bringup: registers exFAT/NTFS + USB hotplug thread
   ├── cellfs.c          # default cellFs + synthetic-root backend
   ├── exfat.c           # hand-written exFAT reader/writer backend
   ├── ntfs.c            # hand-written NTFS reader/writer backend
   ├── http.c            # transport binding + one-shot request/response (malloc-free)
   ├── http-stream.c     # the ring-buffer streaming engine (malloc + prefetch thread)
   ├── transport-cellhttp.c # cellHttp (firmware TLS) transport backend
   ├── cellhttp-stack.c  # one-time firmware http/ssl/https bringup
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
`ftp.c`, `vfs.c`, `vfs-init.c`, `cellfs.c`, `exfat.c`, `ntfs.c`, and the http
module (`http.c`, `http-stream.c`, `transport-cellhttp.c`, `cellhttp-stack.c`)
(`vfs.c` is the filesystem router + mount registry + recursive tree operations,
naming no backend; `vfs-init.c` brings the VFS up — registers exFAT/NTFS and runs
the USB hotplug thread; `cellfs.c` the default HDD/FAT32 backend; `exfat.c` /
`ntfs.c` the removable-media backends; `ftp.c` the shared FTP server; `http.c` the
transport-agnostic client, split from the `http-stream.c` streaming engine so a
PRX that only does one-shot fetches stays malloc-free). The library has no
dependencies on `simple-lib-plugin` or `simple-lib-app` — those depend on it, not
the other way around.
