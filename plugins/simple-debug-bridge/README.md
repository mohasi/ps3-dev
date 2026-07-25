# simple-debug-bridge

The on-console half of the debug bridge. It runs as a VSH plugin, opens a TCP
listener on port **8785** (LAN), and does what the host-side tools ask it to.

Pair it with `tools/debug-bridge-client/`. That host client keeps a live
connection to the PS3 and puts a plain HTTP proxy in front of it at
`http://localhost:8786`, so scripts and the `ps3` MCP tool talk to the console
over ordinary HTTP without knowing the wire framing.

Up to **4 hosts** can be connected at once (the client, the MCP server, ad-hoc
scripts), so nothing has to be shut down to make room. Commands run one at a
time across all of them, and every log line goes to every connected host. A
fifth connection is answered `ERR busy`.

## What it does

- **Forwards logs** from other on-console plugins/apps to the host client's
  **Logs** tab, so you can watch startup output live.
- **Reads memory** from the vsh address space.
- **Transfers files** both ways (pull, push, delete, list, recursive snapshot).
- **Installs / removes VSH plugins** and edits `boot_plugins.txt`.
- **Installs / removes games** from debug-format `.pkg` files.
- **Presses buttons** through a virtual controller (XMB and in-game).
- **Starts and quits titles** (`launch` / `exit-game`).
- **Inspects modules and processes** (list, per-module detail, import tracing).
- **Powers the console** (soft-restart the XMB, hard reboot, shut down).

## Commands

Every command below is implemented and working.

| Command | Description |
|---|---|
| `ping` | heartbeat; the client polls it to detect the connection state |
| `restart-ps3` | hard reboot (`sys_sm_shutdown` mode `0x1200`) |
| `restart-xmb` | soft restart of the XMB — restarts vsh (`sys_sm_shutdown` mode `0x0200`) |
| `shutdown` | power off (`sys_sm_shutdown` mode `0x1100`) |
| `pad <button>[+<button>...] [holdMs]` | press buttons on a virtual controller, then release. `holdMs` defaults to 80, capped at 5000. Names: `up down left right cross circle square triangle l1 l2 r1 r2 l3 r3 start select ps`. |
| `pad hold <buttons>` / `pad release` | keep buttons pressed / let everything go. |
| `pad off` | unregister the virtual controller. It registers itself on first use and stays until then. |
| `launch <TITLE_ID>` | start an installed title: points `/app_home/PS3_GAME` at `/dev_hdd0/game/<TITLE_ID>` (cobra map-paths), then drives the XMB onto that icon and presses it. |
| `exit-game` | quit whatever is running, back to the XMB (`game_plugin` ExitGame). `ERR` if nothing is running. |
| `read-mem <hexAddr> <decLen>` | dump `<decLen>` raw bytes from vsh memory starting at `<hexAddr>`. Payload is binary. |
| `module-list` | one line per PRX loaded into vsh.self: `<id>\t<name>\t<filename>\n`. Includes system modules. |
| `module-info <name>` | per-module detail: ELF segments, linkage tables, exports, imports (sectioned text). |
| `module-trace-on` / `module-trace-off` | arm / disarm import-call tracing. Captures land in a ring buffer on the PS3; pull it later with `pull-file /dev_hdd0/tmp/trace-capture.bin`. |
| `process-list` | processes the bridge can see. On CEX this is always the single `vsh\tlive` line — the cross-process debug syscalls are locked to the calling process. The tab shape is kept so a future CFW escalation can add rows without changing the client. |
| `process-info <name>` | identity + loaded-module list for a process: `pid`, `name`, `sdk`, then one `mod\t…` row per PRX. `<name>` must currently be `vsh`. |
| `pull-file <path> [offset] [length]` | stream raw bytes back. `length=0` (or omitted) means "to end of file". |
| `push-file <path> <size>` | upload `<size>` bytes to `<path>` (truncating). The parent folder must already exist. |
| `delete-file <path>` | remove `<path>`. Missing file still returns `OK`. |
| `list-dir <path>` | one line per entry: `<kind>\t<size>\t<mtime>\t<name>\n`. Not recursive. |
| `stat-tree <root>` | recursive snapshot of `<root>` written to `/dev_hdd0/tmp/stat-tree.txt` as `<kind>\t<size>\t<mtime>\t<sha1>\t<path>\n` per entry (sha1 only for regular files ≤ 256 KiB; larger files emit zeros). Reply is `OK files=<n> dirs=<n> -> …`; pull the file separately. ~40 s on `/dev_hdd0`. |
| `vsh-plugin-install <name> <size>` | upload `<size>` bytes to `/dev_hdd0/plugins/<name>.sprx`, then add an active line to `boot_plugins.txt`. |
| `vsh-plugin-uninstall <name>` | remove every `boot_plugins.txt` line for `<name>`, then delete `/dev_hdd0/plugins/<name>.sprx`. |
| `pkg-install <name> <clean> <size>` | upload `<size>` bytes to `/dev_hdd0/packages/<name>.pkg`, read `TITLE_ID` from its `PARAM.SFO`, optionally wipe `/dev_hdd0/game/<TITLE_ID>/` first if `clean=1`, then extract there. **Debug-format pkgs only.** No XMB install dialog. Run `restart-xmb` after to refresh the Games column. |
| `pkg-uninstall <TITLE_ID>` | recursive delete of `/dev_hdd0/game/<TITLE_ID>/`. |

## Deploying it

`simple-debug-bridge` is the **install transport** — the host client pushes
every other plugin and package through it. So when its own backing libs or
headers change, **redeploy it first**, before anything that depends on it, or
you install through stale code.

To update the bridge itself: FTP the new `.sprx` over the existing one and run
`restart-xmb`. It cannot stop or unload itself in-process, because the thread
that would issue the stop is the one whose code pages get unmapped.

## VSH PRX constraints

`_start()` runs on the loader thread and must return `SYS_PRX_RESIDENT`
promptly — no blocking work inline. So `_start` only spawns the worker thread
and returns; the accept loop, sockets, and all blocking work live on that
thread.

**Order matters in `_start`.** `startServer()` runs first — it creates the
locks and spawns the worker thread — and only then does `setLogCallback()`
install the log sink. Installing the sink first, before the thread that owns
its lock exists, once deadlocked `_start` (both sides waiting on each other
forever) and silently killed the plugin.

The worker thread waits for the XMB to be ready before it binds the port. The
VFS (the exFAT/NTFS drivers) is intentionally **not** brought up: the bridge's
file commands only touch cellFs (HDD, dev_flash, kernel FAT32 USB), and leaving
the VFS out keeps those drivers from bloating this `.sprx`. Reach exFAT/NTFS
sticks via FTP or the file-manager instead.

## Protocol

One persistent duplex TCP socket per host, up to 4 of them; commands run one at
a time across all hosts. Each reply is length-framed:

```
request:  <command> [args...]\n
response: <STATUS> <n>\n<n bytes>
```

`<STATUS>` is `OK` or `ERR`. `<n>` is the exact byte length of the payload that
follows (no trailing newline). Text payloads are UTF-8 / ASCII; binary payloads
(`pull-file`, `read-mem`) are raw bytes. An empty payload is `OK 0\n`.

### Producer log forwarding

The same listener also accepts **producer** connections from other on-console
plugins/apps. A producer's first line is the handshake:

```
REGISTER <plugin|app> <name>\n
```

After that the producer streams `LOG <n>\n<n bytes>` frames whenever its
`dbg.h` sink fires. The bridge forwards each one to the connected host as an
out-of-band `LOG` frame, and the host renders it in the **Logs** tab. If no
host is connected, the bridge holds producer lines in a bounded ring buffer and
replays them on the next host-connect, so the cold-boot window is not lost.

The bridge tees its own logs the same way, via `setLogCallback(forwardLogToHost)`.

Producers integrate through `bridge.h` in `simple-lib-plugin`:
`registerWithBridge("plugin", "ftp")` from `_start()` installs the sink and
spawns a background thread that handles connect + REGISTER + backlog drain, so
registration never blocks startup.

### Binary upload framing

Upload commands (`push-file <path> <size>`, `vsh-plugin-install <name> <size>`,
`pkg-install <name> <clean> <size>`) end their command line with a byte count,
then send exactly that many raw bytes right after the `\n`:

```
request:  <command> <args...> <byte-count>\n
request:  <byte-count raw bytes>
response: <STATUS> <n>\n<n bytes>
```

The plugin reads exactly `<byte-count>` bytes into the destination file before
replying.

### boot_plugins.txt editing

`vsh-plugin-install` / `vsh-plugin-uninstall` both collapse to one primitive:
`setPluginLine(name, state)` where `state ∈ { ACTIVE, ABSENT }`.

A line "matches plugin `<name>`" when its trimmed form, after stripping any
leading `#`, **ends with `/<name>.sprx`** (basename match — path-agnostic). The
primitive reads the file, drops every matching line, then appends the canonical
`/dev_hdd0/plugins/<name>.sprx` line (active) or nothing (absent), and writes
back. Result: at most one line per plugin, in the canonical path, with stale
variants collapsed away.

## Layout

```
simple-debug-bridge/
├── src/
│   └── prx.c               # plugin entry — _start / _stop only
├── include/
│   ├── server.h            # accept loop, host/producer demux, log forwarding, dispatch
│   ├── cmd-common.h        # shared reply / parse helpers used by every cmd-*
│   ├── cmd-introspect.h    # module-list, process-list, process-info
│   ├── cmd-file.h          # pull-file / push-file / delete-file / list-dir
│   ├── cmd-stat-tree.h     # recursive sha1'd snapshot for install diffs
│   ├── cmd-pad.h           # virtual controller (ldd pad register + button frames)
│   ├── cmd-game.h          # launch / exit-game via the xmb's own plugins
│   ├── cmd-trace.h         # module-trace-on / module-trace-off
│   ├── cmd-read-mem.h      # read-mem (raw vsh memory dump)
│   ├── fileio.h            # socket-coupled file streaming
│   ├── plugin.h            # vsh plugin install/uninstall + boot_plugins.txt edits
│   └── pkg.h               # debug-format .pkg parser + extractor
├── simple-debug-bridge.vcxproj
└── README.md
```

Generic file primitives (`readFile`, `writeFile`, `fileExists`, `makeDir`,
`deleteFile`) and syscall wrappers (`sysPower`, `prxFinalizeSelf`) live in
`libs/simple-lib-plugin` and are shared across plugins.

## Teardown

`_stop` calls `stopServer()` (signals the accept loop, wakes `accept()` and any
in-progress recv via `shutdown()`, and joins the worker thread) then
`prxFinalizeSelf()`. Only relevant when something calls `sys_prx_stop_module`
on the PRX (hot reload during dev). On power-off / reboot LV2 tears everything
down externally.

## Port

`8785` — derived from `8000 + (fnv1a32("simple-debug-bridge") % 1000)`.

## Testing

Direct to the PS3:

```
echo "ping" | nc <ps3-ip> 8785
echo "restart-xmb" | nc <ps3-ip> 8785
```

Through the host HTTP proxy (with `debug-bridge-client` running):

```
curl http://localhost:8786/ping
curl "http://localhost:8786/pull-file?path=/dev_hdd0/tmp/dbg.txt&text=1"
curl "http://localhost:8786/delete-file?path=/dev_hdd0/tmp/old.txt"
```
</content>
</invoke>
