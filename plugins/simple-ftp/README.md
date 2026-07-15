# simple-ftp

A small FTP server plugin for PlayStation 3 CFW, packaged as a VSH-injected PRX. It exposes the console's filesystem over FTP so you can transfer files from a PC with WinSCP, FileZilla, lftp, curl, or rclone. Anonymous login, passive mode only, binary transfers.

## Why this exists

The goal is to be as simple as possible — one job, no bloat. An FTP plugin should add an FTP server and nothing else. After installing `webftp_server_lite.sprx` once and discovering it quietly created directories, injected menu entries, and reached well beyond its name, I wanted something that would load, listen on port 21, and stay out of the way. That's what this is. No XMB entries, no background scans, no side effects on the filesystem beyond what an FTP client explicitly asks for.

## What it does

The plugin starts with the VSH, waits for the XMB to be ready (so the network is up), then starts the shared `simple-lib-core` FTP server on port 21. On startup the server best-effort mounts `/dev_blind` so that `/dev_flash` (the console's internal system storage) is writable over the connection — the same "Enable /dev_blind on startup" behaviour webMAN-MOD exposes as an option. Once you're connected you can browse, download, upload, resume, append, delete, rename, and make directories anywhere on the filesystem.

Only cellFs-backed storage is exposed: `/dev_hdd0` and FAT32 USB sticks. This plugin does not bring up the shared VFS, so exFAT- and NTFS-formatted USB drives are not readable or writable over the connection, and none of that driver code is linked into the build.

## Installation

Copy `simple-ftp.sprx` into `/dev_hdd0/plugins/` on the console, then add a line pointing at it to `/dev_hdd0/boot_plugins.txt`:

```
/dev_hdd0/plugins/simple-ftp.sprx
```

Reboot the console. The plugin loads with the VSH and starts listening immediately. Connect from the PC to the PS3's LAN address (10.0.0.2 here) on port 21, any username, any password.

Port 21 is a hard requirement — if another FTP server is already bound there (webMAN-MOD ships one on by default; IRISMAN ships one but it's off unless you enable it), this plugin's listener can't bind and its thread exits. Disable or remove the other server first.

## How it works

The PRX side is deliberately thin: it registers with the debug bridge, waits for the XMB, and calls the shared `simple-lib-core` FTP startup on port 21. It never calls `initVfs`, so the exFAT/NTFS backends and the VFS hotplug poll thread never link in or run — the server still routes every path through the shared VFS, but only the built-in cellFs route is ever reachable.

The shared server owns one listener thread and a fixed pool of two session slots, each with its own worker thread. A session thread parses the control channel, opens an OS-assigned passive (PASV) data socket per transfer, and runs until the client sends `QUIT` or the connection drops. Two concurrent sessions are supported — one is too tight for WinSCP, which opens a second control channel for its edit-in-place flow.

Both listing styles are implemented: the modern `MLSD` / `MLST` (RFC 3659, what every current client prefers) and the classic `LIST` / `NLST`. `FEAT` also advertises `SIZE`, `MDTM`, `REST STREAM` (resume), and `UTF8`. Timestamps come from the Cell real-time clock rather than libc's `time()`/`gmtime()`, which don't reliably work from a PRX.

The protocol behaviour was cross-checked against IRISMAN and webMAN-MOD during development, but no code was copied from either — this is a fresh implementation.

## Socket-exhaustion handling

The PS3's network stack holds only about 460 sockets, and every finished transfer leaves its data socket sitting in a "TIME_WAIT" cool-down for roughly 60 seconds before the stack reclaims it. Pulling many small files back-to-back fills the pool with cooling-down sockets faster than they drain, and the stack then starts refusing new connections — eventually even the control connection, which wedges the whole session.

Two dead ends ruled out the easy fixes. Slamming each finished transfer shut with an abrupt reset (RST) broke downloads, because a clean close is what tells the client the file ended — the side that closes cleanly is always the one that owns the cool-down. And simply waiting for sockets to drain doesn't help, because the cool-down lives on the PS3 regardless.

The shipped fix is token-bucket pacing on new data connections. A burst of 64 transfers runs at full speed, after which the rate settles to about 5 new data connections per second — slow enough that the number of cooling-down sockets stays well under the pool, with headroom left for the control connection. Bursty or light use never touches the burst budget and pays zero added latency; only sustained hammering gets throttled, and the server never wedges. On top of that, opening a passive port retries with a short backoff while the pool is momentarily dry, so a transient shortage throttles the rate instead of failing the transfer.

## Memory footprint

The two session slots dominate the cost: each `FtpSession` is about 43 KB (a 32 KB transfer buffer, an 8 KB listing-staging buffer, and a few 1 KB command/path/rename buffers), so roughly 86 KB of fixed memory whether or not anyone is connected. Because this plugin never calls `initVfs`, the exFAT and NTFS backends and the VFS hotplug thread add nothing. The listener thread has a 6 KB stack; the startup thread keeps a 16 KB stack and exits once the server is up; an actively connected client's session thread adds another 16 KB of stack.

## Stability and speed

Multi-file delete is stable here. On both IRISMAN and webMAN-MOD the same operation hangs partway through — selecting a batch in WinSCP and pressing delete doesn't always finish. webMAN-MOD also doesn't support WinSCP's double-click edit flow (edit-in-place over a fetch-save round-trip); that works in this plugin.

Throughput lands at roughly 90% of IRISMAN's, the fastest of the three in testing; webMAN-MOD is noticeably slower day-to-day. The speed comes from a few small choices: transfer chunks sized to match the socket buffers (a bigger 1 MB socket buffer actually made it slower), OS-assigned passive ports with no retry loop on the happy path, and shutting sockets down cleanly on teardown so blocking reads and accepts unstick.

## Building and deploying

Build and deploy through the ps3 MCP tool (`build` kind `plugins`, name `simple-ftp`; then `deploy`). The PS3's LAN address is 10.0.0.2.

## Debug log

Anything unusual — bind conflicts, accept errors, malformed commands, failed mounts — is logged via `dbg.h` (`logInfo` / `logWarn` / `logError`), one timestamped, level-prefixed line per event.

Every line is written to `/dev_hdd0/tmp/dbg.txt` and, if `simple-debug-bridge` is installed, forwarded live to the `debug-bridge-client` Logs tab on the PC. The plugin registers with the bridge from `_start()`, so startup chatter is buffered locally and sent as soon as the bridge link comes up — even early-boot races reach the host.
