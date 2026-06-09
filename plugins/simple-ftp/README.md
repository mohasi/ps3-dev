# simple-ftp

A small FTP server plugin for PlayStation 3 CFW, packaged as a VSH-injected PRX. It exposes the console's filesystem over FTP so you can transfer files from a PC with WinSCP, FileZilla, lftp, curl, or rclone. Anonymous login, passive mode only, binary transfers.

## Why this exists

The goal is to be as simple as possible — one job, no bloat. An FTP plugin should add an FTP server and nothing else. After installing `webftp_server_lite.sprx` once and discovering it quietly created directories, injected menu entries, and reached well beyond its name, I wanted something that would load, listen on port 21, and stay out of the way. That's what this is. No XMB entries, no background scans, no side effects on the filesystem beyond what an FTP client explicitly asks for.

## What it does

The plugin starts with the VSH, waits for XMB readiness, then starts the shared `simple-lib-core` FTP server on port 21. The shared server best-effort mounts `/dev_blind` so that `/dev_flash` is writable over the connection (the same "Enable /dev_blind on startup" behaviour webMAN-MOD exposes as an option). Once you're connected you can browse, download, upload, delete, rename, and make directories anywhere on the filesystem. Every error path logs a timestamped line to `/dev_hdd0/tmp/dbg.txt` — if something goes wrong, that's the first place to look.

## Installation

Copy `simple-ftp.sprx` into `/dev_hdd0/plugins/` on the console, then add a line pointing at it to your VSH plugins list in `/dev_hdd0/boot_plugins.txt`:

```
/dev_hdd0/plugins/simple-ftp.sprx
```

Reboot the console. The plugin loads with the VSH and starts listening immediately.

Port 21 is a hard requirement — if another FTP server is already bound there (webMAN-MOD ships one on by default; IRISMAN ships one but it's off unless you enable it), this plugin's `bind()` will fail and the listener thread will exit. Disable the other server first, or remove it from `boot_plugins.txt`, before installing this one.

## How it works

The PRX side is deliberately thin now: it registers with the debug bridge, waits for XMB, and calls the shared `simple-lib-core` FTP startup API on port 21. The shared server owns one listener thread, a fixed two-entry session pool, and a dedicated PPU thread per accepted client. Each session thread parses the control channel, opens an OS-assigned PASV data socket per transfer, and runs until the client sends `QUIT` or the connection drops. Up to two concurrent sessions are supported — one is too tight for WinSCP, which opens a second control channel for its editor flows.

Directory listings are RFC 3659 only (`MLSD` / `MLST` / `MDTM`). `LIST` and `NLST` aren't implemented. Every modern client prefers MLSD when `FEAT` advertises it, and dropping the old listing formats removes a lot of ls-style formatting and timezone bookkeeping that MLSD doesn't need. Timestamps are sourced from the Cell RTC rather than libc's `time()`/`gmtime()`, which don't reliably resolve from a PRX.

The protocol behaviour was cross-checked against IRISMAN and webMAN-MOD during development, but no code was copied from either — this is a fresh implementation.

## Memory footprint

The static session pool is the dominant cost: two `FtpSession` structs at roughly 141 KB each (128 KB transfer buffer, 8 KB MLSD staging buffer, 3 KB of command/path/rename-from buffers), so about 282 KB of BSS regardless of whether anyone's connected. On top of that, the listener thread has a 6 KB stack and the startup plugin thread a 16 KB stack. When a client is actively connected, its session thread adds another 16 KB of stack. The PRX code segment itself is small.

At rest: about 300 KB. With two concurrent clients: about 330 KB.

## Stability and speed

Multi-file delete is stable here. On both IRISMAN and webMAN-MOD the same operation hangs partway through — selecting a batch in WinSCP and pressing delete doesn't always finish. webMAN-MOD also doesn't support WinSCP's double-click edit flow (edit-in-place over a fetch-save round-trip); that works in this plugin.

Throughput lands at roughly 90% of IRISMAN's, which is the fastest of the three in testing. webMAN-MOD is noticeably slower in day-to-day use. The snappiness comes from a few small design choices: 128 KB transfer chunks sized to match the socket buffers (a 1 MB socket buffer actually regressed us), OS-assigned PASV ports with no retry loop, and `shutdown()` + `socketclose()` to unstick blocking `recv`/`accept` calls on teardown.

## Debug log

Anything unusual — bind conflicts, accept errors, malformed commands, failed mounts — is logged via `dbg.h` (`logInfo` / `logWarn` / `logError`), one timestamped, level-prefixed line per event.

Every line is written atomically to `/dev_hdd0/tmp/dbg.txt` **and**, if `simple-debug-bridge` is installed, forwarded live to the `debug-bridge-client` Logs tab on the PC. The plugin calls `registerWithBridge("plugin", "simple-ftp")` from `_start()`, so startup chatter is buffered locally and drained as soon as the bridge link comes up — even early-boot races reach the host.
