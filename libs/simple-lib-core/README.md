# simple-lib-core

The shared foundation every other library and program in this workspace builds on. It is
context-neutral: the same archive links into VSH plugins (`simple-lib-plugin`, PRX context) and
into apps (`simple-lib-app`). Built with `-fno-builtin-printf -nodefaultlibs` so it never pulls in
the system C library.

## The one hard rule: no libc, no malloc

The parts a VSH plugin links must not depend on the system C library or on `malloc`. That is why
this library ships its own string helpers, its own `printf`, its own file layer, and so on — a
plugin that links an unresolved libc symbol dies silently when the console loads it.

The rule is enforced per file, not per library. The light headers and the light compiled units are
malloc-free and PRX-safe. A few heavier units (the ZIP support and the streaming half of the http
client) do allocate and are meant for apps only. They live in their own files, so a plugin that
never calls them never drags the heap in. Each entry below says which side it is on.

## What it provides

### Text and formatting (header-only, PRX-safe)

- **string-utilities.h** — the replacement for `string.h`. Length, bounded copy, memory copy/set,
  equality and case-insensitive compare, prefix/suffix tests, upper/lower, UTF-8 truncation, path
  normalisation, byte search, URL encode/decode, XML escaping, integer/IPv4/date formatting, and
  UTF-8 ↔ UTF-16 conversion. Always reach for this before writing your own — check here first to
  avoid duplicates.
- **path.h** — path helpers: `joinPath`, `toParentPath`, `getParentPath`, `getBaseName`,
  `getExtension`, `deviceRootOf`, `isValidFileName`, plus `MAX_PATH_LEN`.
- **format.h** — human-readable byte sizes (`formatSize`, `formatSizeApprox`).
- **text-sanitize.h** — clean up untrusted text: decode XML entities, strip markup tags, remove
  invisible marks, flatten runs of whitespace.
- **sfo.h** — read a value out of a PARAM.SFO metadata blob (`getSfoValue`).
- **sha1.h** — a small SHA-1 hash (`hashSha1` and the streaming `init/update/finalize` form).

### Files and storage

- **vfs.h** — the one filesystem layer every consumer goes through. **Use this, never cellFs
  directly.** A single `/`-rooted namespace where console devices (`/dev_hdd0`, kernel-mounted
  FAT32 `/dev_usb000`) and our own exFAT / NTFS mounts (`/exfat0`, `/ntfs0`, …) sit side by side;
  each call is routed to the right backend. High-level helpers — `readFile`, `writeFile`,
  `deleteFile`, `copyFile`, `copyTree`, `moveTree` — plus the low-level ops `statPath`,
  `openDir`/`readDir`/`closeDir`, `openFs`/`readFs`/`writeFs`/`seekFs`/`fsyncFs`, `renamePath`,
  `makeDirPath`, `removeFilePath`/`removeDirPath`, `getFreeSpace`, `listMounts`, `getScheme`. The
  cancellable, byte-reporting tree operations (`measureTree`, `copyTreeProgress`,
  `deleteTreeProgress`, `mergeTreeProgress`, `countTreeConflicts`) live in `vfs.c` and each flush
  the volume once at the end for crash-durability.
- **vfs-init.h / vfs-init.c** — bringup: `initVfs` registers the exFAT and NTFS backends and starts
  the USB hotplug poll thread; `shutdownVfs` stops it. Kept in a separate file so a consumer that
  only needs to route and read paths links `vfs.c` alone and never pulls the exFAT/NTFS drivers
  (this linker cannot strip unreachable code). Call `initVfs` from the host's own thread, never on
  a request-serving path.
- **tree-walk.h** — recursive directory walk with a visitor callback and a cancel flag (`walkTree`).
- **exfat.h** — hand-written, libc-free exFAT reader/writer for USB volumes: mount (superfloppy,
  MBR- and GPT-partitioned), list, stat, free space, read/write/create/delete/rename with real
  timestamps. Registers as a VFS backend via `initExfat()`. No formatting. Validated with
  exfatprogs `fsck`.
- **ntfs.h** — hand-written, libc-free NTFS reader/writer for USB volumes, same shape as exFAT.
  Registers via `initNtfs()` (claims only `NTFS`-labelled volumes). No formatting. Validated
  against libntfs-3g.
- **usb-storage.h** — shared USB mass-storage device identity/presence layer used by the hotplug
  detection and both removable-media backends.
- **cellfs.h** — the default console-filesystem backend (HDD + FAT32 USB) behind the VFS.
- **settings-file.h** — tiny `key=value` settings files with `#` comments. `loadSettingsFile`
  auto-creates the file from a defaults string on first run; `findSettingValue` /
  `settingValueEquals` read a key back.
- **dir-playlist.h** — list a folder's files through a filter (`listDirFiltered`) and step forward/
  back through them (`playlistOpen`, `playlistStep`), used by the media players.
- **zip.h / zip.c** — ZIP read/write and deflate/inflate, a trimmed copy of miniz 3.1.2, plus our
  own VFS-bridging wrappers `zipPathsProgress`, `unzipArchiveProgress`, `measureZipSource`,
  `measureZipArchive` (same cancel/progress convention as the tree ops; archive entry names that
  would escape the destination are rejected as untrusted input). **Apps-only** — miniz allocates.

### Networking

- **http.h** — a transport-agnostic HTTP(S) client. One API: `fetchHttp` / `getHttp` for a one-shot
  request/response (`fetchHttpCapturing` also hands back one named response header, e.g. the `Location`
  of a Google Drive upload session), `openHttpStream` / `readHttpStream` / `seekHttpStream` / `getHttpStreamSize` /
  `closeHttpStream` for seekable, never-fully-downloaded media streaming. The actual TLS transport
  is bound once at startup: `initSystemHttp()` uses the console's firmware TLS (cellHttp — free
  weight, reaches RSA hosts like Google/YouTube); `initModernHttp()` (from **simple-lib-https**)
  swaps in BearSSL and additionally reaches ECDSA-only hosts. `shutdownHttp()` drops the idle
  keep-alive pool at exit. The light half (transport binding + one-shot, `http.c` + the cellHttp
  backend in `transport-cellhttp.c` / `cellhttp-stack.c`) is malloc-free; the streaming engine
  (4 MB ring + prefetch thread, `http-stream.c`) is a separate file pulled in only by a caller that
  references `openHttpStream`, so a plugin that only does one-shot fetches never brings the heap
  into a VSH PRX. The engine asks an ordinary server for everything from the read position on, with a
  `Range` header. YouTube's media hosts no longer answer that (the reply is a redirect whose target is
  refused), so for those it asks for one window at a time as a `&range=start-end` query parameter, sized
  and paced from the stream's own byte rate because they only serve so far ahead of playback.
- **network.h** — `getLocalIpv4()` resolves the console's primary IPv4 address.
- **ftp.h / ftp.c** — the shared anonymous FTP server, run as a singleton (`startFtpServer`,
  `stopFtpServer`, `isFtpServerRunning`, `isFtpPortAvailable`). Usable from both apps and plugins;
  serves the whole VFS, so USB exFAT/NTFS volumes are reachable over FTP.
- **wire.h** — framed TCP helpers shared by the debug-bridge server and client: `sendBytes`,
  `sendFrameHeader`, `sendFrame` (formats `<verb> <len>\n<bytes>`), `receiveLine`.

### Runtime, logging, and the debug bridge

- **syscall.h** — the whole LV2 syscall layer (header-only, PRX-safe). Generic inline-asm
  trampolines (`scCall1..scCall7`); the filesystem syscalls `mountDevBlind` (837, cobra
  `/dev_blind` mount) and `syncDevice` (839, flush a volume to disk); and the prx/power/memory
  wrappers `sysPower` (379), `getPrxModuleIdByAddress` (461), `prxFinalizeSelf` (482),
  `prxList`/`prxName`/`prxInfo`/`prxLinkage` (494/495), and `sysMemAllocate`/`sysMemFree` (348/349).
  Note: `sysMemAllocate` rounds a size up to the 64 KB page granularity — sizes are not rounded for
  you, so round before calling. (VSH-only NID stubs stay in `simple-lib-plugin/vsh.h`.)
- **disc-mount.h / disc-mount.c** — mounting an `.iso` as the Blu-ray disc through Cobra's syscall 8:
  `mountDiscImage` (fake eject, mount, fake insert, and remember the path), `unmountDiscImage`,
  `isDiscImageMounted`, `getRealDiscType`, plus `getLastMountedImage` / `forgetLastMountedImage` for
  the remembered path. Shared by the `simple-disc-mount` plugin's XMB menu and the file manager's X
  action. Needs Cobra CFW; the image must live on a volume lv2 itself mounts (internal drive or
  FAT32 USB), since Cobra reads it kernel-side.
- **thread.h** — `spawnThread()` PPU-thread spawn helper and stack-size constants.
- **dbg.h / dbg.c** — leveled, timestamped logging: `logInfo` / `logWarn` / `logError`. Each call
  writes one prefixed line to `/dev_hdd0/tmp/dbg.txt` with a single atomic `cellFsWrite`. An
  optional sink (`setLogSink`) forwards each fully-formatted line after the disk write. Defines
  `BacklogLine` / `LOG_LINE_MAX` used by the pre-connect log rings.
- **log-backlog.h** — a small ring that holds log lines produced before a connection exists:
  `pushLogBacklog` (overwrites the oldest on overflow), `drainLogBacklog` (replays in order).
- **bridge-client.h** — the producer side of the debug bridge: `registerWithBridge("plugin"|"app",
  "<name>")` installs the log sink, then a background thread connects to the bridge on
  `localhost:8785`, registers, and drains the backlog so early-startup lines still reach the host.

## Layout

```
simple-lib-core/
├── simple-lib-core.vcxproj
├── include/   # the headers listed above
└── src/
   ├── vfs.c                # path router + mount registry + cancellable tree ops (names no backend)
   ├── vfs-init.c           # VFS bringup: registers exFAT/NTFS + USB hotplug thread
   ├── cellfs.c             # default HDD/FAT32 backend
   ├── exfat.c              # hand-written exFAT backend
   ├── ntfs.c               # hand-written NTFS backend
   ├── zip.c                # trimmed miniz + VFS zip/unzip wrappers (apps-only)
   ├── http.c               # transport binding + one-shot request/response (malloc-free)
   ├── http-stream.c        # ring-buffer streaming engine (malloc + prefetch thread)
   ├── transport-cellhttp.c # firmware-TLS transport backend
   ├── cellhttp-stack.c     # one-time firmware http/ssl/https bringup
   ├── ftp.c                # shared FTP server
   ├── network.c            # local IPv4 lookup
   ├── dir-playlist.c       # folder listing + step
   ├── tree-walk.c          # recursive directory walk
   ├── dbg.c                # logging
   └── printf.c             # libc-free snprintf/vsnprintf/sprintf
```

## Usage

1. Build `simple-lib-core` first — produces `libsimple-lib-core.a` in `bin/Release/`.
2. In your plugin / app vcxproj:
   - Add `$(SolutionDir)libs\simple-lib-core\include` to the include directories.
   - Place the `.a` **before** the SDK stubs in `AdditionalDependencies` — our printf symbols must
     resolve before `libc_stub.a`, or a PRX silently fails to load in VSH context.
3. Include headers as needed: `#include "vfs.h"`, `#include "dbg.h"`, `#include "http.h"`, etc.

## Design

Most utilities are `static inline` headers that compile straight into the consumer. The compiled
units are the ones listed under `src/` above. The library depends on nothing above it —
`simple-lib-plugin` and `simple-lib-app` depend on it, never the other way around.
