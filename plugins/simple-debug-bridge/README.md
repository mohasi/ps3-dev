# simple-debug-bridge

VSH plugin that opens a TCP listener on port **8785** (LAN) so a PC client can
remotely control the PS3 for development and debugging.

Pair with the WPF companion in `tools/debug-bridge-client/`.

## Commands

| Command | Status | Description |
|---|---|---|
| `ping` | ✅ | heartbeat (also used by client to detect connection state) |
| `restart-ps3` | ✅ | LV2 hard reboot (`sys_sm_shutdown` mode `0x1200`) |
| `restart-xmb` | ✅ | LV2 soft reboot — restarts vsh (`sys_sm_shutdown` mode `0x0200`) |
| `shutdown` | ✅ | power off (`sys_sm_shutdown` mode `0x1100`) |
| `capture <x> <y> <w> <h>` | ✅ | capture region of front buffer as raw ARGB8888. **XMB only** — returns `ERR` in-app. Direct framebuffer reads + RSX FIFO pause are only safe when VSH owns RSX; in-app capture (via Sony's screenshot hook + file readback) is a follow-up that will share this same command. Use `display-info` first for geometry. |
| `display-info` | ✅ | report `<width> <height> <pitch> <depth>` of current front buffer. |
| `terminate` | 🔲 | kill running game process |
| `launch <titleid>` | 🔲 | launch installed title |
| `input <buttons>` | 🔲 | send fake pad input |
| `module-list` | ✅ | list every PRX loaded into vsh.self via `sys_prx_get_module_list` (syscall 494) + `sys_prx_get_module_info` (syscall 495). Payload is one line per module: `<id>\t<name>\t<filename>\n`. Includes system modules — useful for debugging. |
| `module-info <name>` | ✅ | per-module detail: ELF segments, linkage tables, exports, imports. Payload is sectioned text consumed by the Modules tab. Foundation for trace tooling. |
| `module-trace-on` / `module-trace-off` | ✅ | arm / disarm import-call tracing across loaded modules. Captures go to a ring buffer on the ps3, pulled later with `pull-file /dev_hdd0/tmp/trace-capture.bin`. |
| `read-mem <hexAddr> <decLen>` | ✅ | dump `<decLen>` raw bytes from vsh address space starting at `<hexAddr>`. Payload is binary; use `/read-mem?<hex>&<dec>` over HTTP for `application/octet-stream`. |
| `process-list` | ✅ | list processes the bridge can introspect. Always emits a single `vsh\tlive` line: on CEX the dbg syscalls for cross-process enumeration (`sys_dbg_get_process_list` and friends) are either ENOSYS or locked to the calling pid, so the bridge can only see the process it runs in. The tabular wire shape (`<name>\t<status>\n` per row) is preserved so a future CFW escalation (DEX / Rebug / QA flags) can add entries without changing the client. |
| `process-info <name>` | ✅ | per-process identity and loaded-module list. Payload is tab-separated text: `pid\t0x<hex>`, `name\t<text>`, `sdk\t0x<hex>` (from `sys_process_get_sdk_version`), then one `mod\t<id>\t<name>\t<filename>` row per PRX loaded into the process. Currently `<name>` must be `vsh`. Imports/exports of the main `.self` itself are not yet surfaced (followup: parse `/dev_flash/vsh/module/vsh.self`). |
| `vsh-plugin-load <name> <size>` | 🔲 | upload `<size>` bytes to `/dev_hdd0/tmp/sdb/<name>.sprx`, then load + start. Replaces if already loaded by same name. |
| `vsh-plugin-load <ps3-path>` | 🔲 | load + start a VSH PRX already on disk (no upload). Path may be quoted (`"..."`) to be unambiguous. |
| `vsh-plugin-unload <name>` | 🔲 | stop + unload a VSH PRX by module name |
| `vsh-plugin-enable <name>` | 🔲 | ensure exactly one active line for `<name>` in `boot_plugins.txt` (canonical path `/dev_hdd0/plugins/<name>.sprx`) |
| `vsh-plugin-disable <name>` | 🔲 | ensure exactly one commented line for `<name>` in `boot_plugins.txt` |
| `vsh-plugin-install <name> <size>` | ✅ | upload `<size>` bytes to `/dev_hdd0/plugins/<name>.sprx`, then `enable`. Thin wrapper over `push-file` + `setPluginLine(ACTIVE)`. |
| `vsh-plugin-uninstall <name>` | ✅ | remove every line referencing `<name>` from `boot_plugins.txt`, then `delete-file /dev_hdd0/plugins/<name>.sprx`. Thin wrapper over `setPluginLine(ABSENT)` + `delete-file`. |
| `pkg-install <name> <clean> <size>` | ✅ | upload `<size>` bytes to `/dev_hdd0/packages/<name>.pkg`, parse `PARAM.SFO` for `TITLE_ID`, optionally wipe `/dev_hdd0/game/<TITLE_ID>/` if `clean=1`, then extract the pkg there. **Debug-format pkgs only** (`pkg_rev_type == 0x00000001`). No XMB install dialog. Reply: `OK installed <TITLE_ID> (<n> files, <bytes> bytes)`. Run `restart-xmb` after to refresh the Games column. |
| `pkg-uninstall <TITLE_ID>` | ✅ | recursive delete of `/dev_hdd0/game/<TITLE_ID>/`. Reply: `OK uninstalled <TITLE_ID> (<bytes> bytes)`. |
| `pull-file <path> [offset] [length]` | ✅ | stream raw bytes back. Payload is the requested file window. `length=0` (or omitted) means "to end of file". |
| `push-file <path> <size>` | ✅ | upload `<size>` bytes to `<path>` (truncating). Parent directory must already exist — use a separate `mkdir`-equivalent if needed. |
| `delete-file <path>` | ✅ | unlink `<path>`. Idempotent: missing file returns `OK`. |
| `list-dir <path>` | ✅ | one tab-separated line per entry: `<kind>\t<size>\t<mtime>\t<name>\n`. Non-recursive. Used to diff `/dev_hdd0/` around a sony-side install. |
| `stat-tree <root>` | ✅ | recursive snapshot of `<root>` written to `/dev_hdd0/tmp/stat-tree.txt` as `<kind>\t<size>\t<mtime>\t<sha1>\t<path>\n` per entry. sha1 only for regular files <= 256 KiB; larger files emit `0`s and diff on size+mtime. Reply is just `OK files=<n> dirs=<n> -> /dev_hdd0/tmp/stat-tree.txt`; pull the file separately. Runs ~40 s on `/dev_hdd0`. Iterative DFS, single 64 KiB heap allocation, never recurses or follows symlinks. |

## Protocol

Persistent duplex TCP socket. One host client at a time; commands are issued
sequentially on the same connection. Each reply is length-framed:

```
request:  <command> [args...]\n
response: <STATUS> <n>\n<n bytes>
```

The same listener also accepts **producer** connections from on-console
plugins/apps. A producer's first line is the handshake:

```
REGISTER <plugin|app> <name>\n
```

After that the producer streams `LOG <n>\n<n bytes>` frames whenever its
`dbg.h` sink fires. The bridge forwards each frame to the connected host as
an out-of-band `LOG <n>\n<n bytes>` frame; the host renders it in the
**Logs** tab. If no host is connected, the bridge buffers producer log
lines in a bounded ring and replays them on the next host-connect, so the
cold-boot window is not lost.

Producers integrate via `bridge.h` in `simple-lib-plugin`:
`registerWithBridge("plugin", "simple-ftp")` from `_start()` installs the
sink synchronously and spawns a background thread that handles connect +
REGISTER + backlog drain. Plugin work runs in parallel — registration
never blocks startup.

`<STATUS>` is `OK` or `ERR`. `<n>` is the exact byte length of the payload
that follows (no trailing newline). Text payloads are UTF-8 / ASCII; binary
payloads (e.g. `capture`, `pull-file`) are raw bytes. An empty payload is
`OK 0\n`.

Server-side helpers in `server.h`:
- `sendReply(fd, status, msg)` — string payload, length computed internally.
- `sendStreamReply(fd, status, n)` — writes only the header for a known-size
  streamed body, followed by the caller's `sendBytes(...)` of `n` bytes.

Both produce identical wire framing; the split is only to avoid buffering
large bodies (e.g. `module-list` records, file streams) before sending.

### Binary upload framing

Commands that ship a binary payload (e.g. `vsh-plugin-install <name> <size>`,
`push-file <path> <size>`) end their command line with a byte count, then send
exactly that many raw bytes immediately after the `\n`:

```
request:  <command> <args...> <byte-count>\n
request:  <byte-count raw bytes>
response: <STATUS> <n>\n<n bytes>
```

The presence of a numeric `<byte-count>` as the **last** whitespace-separated
argument is what selects the upload form of a command. A trailing on-disk path
(for `vsh-plugin-load <ps3-path>`) is never numeric — and may be wrapped in
double quotes (`"..."`) to be explicit. The plugin reads exactly
`<byte-count>` bytes into the destination file before responding.

### `boot_plugins.txt` editing rules

`enable` / `disable` / `install` / `uninstall` all collapse to one
primitive: `setPluginLine(name, state)` where `state ∈ { ACTIVE, COMMENTED, ABSENT }`.

A line "matches plugin `<name>`" iff its trimmed form, after stripping any
leading `#` and surrounding whitespace, **ends with `/<name>.sprx`**
(basename match — path-agnostic).

The primitive:

1. Read `boot_plugins.txt` line-by-line into memory.
2. Drop **every** line that matches `<name>`.
3. If `state == ACTIVE`, append `/dev_hdd0/plugins/<name>.sprx`.
4. If `state == COMMENTED`, append `# /dev_hdd0/plugins/<name>.sprx`.
5. If `state == ABSENT`, append nothing.
6. Write back.

Per-command:

| Command | `setPluginLine` call | Plus |
|---|---|---|
| `install` | `(name, ACTIVE)` | write `<bytes>` to `/dev_hdd0/plugins/<name>.sprx` |
| `uninstall` | `(name, ABSENT)` | delete `/dev_hdd0/plugins/<name>.sprx` |
| `enable` | `(name, ACTIVE)` | — |
| `disable` | `(name, COMMENTED)` | — |

Effects:
- After any of these commands, `boot_plugins.txt` has at most one line for `<name>`, in the canonical `/dev_hdd0/plugins/` path, in the requested state.
- Multiple stale variants (commented, alternate paths, duplicates) all collapse into a single canonical entry.
- `disable` then `enable` is idempotent (re-adds a fresh active line).
- `uninstall` wipes both file and all references — clean slate.

### Upload destinations

| Command | Destination |
|---|---|
| `vsh-plugin-load <name> <size>` | `/dev_hdd0/tmp/sdb/<name>.sprx` (transient — `tmp` is volatile) |
| `vsh-plugin-install <name> <size>` | `/dev_hdd0/plugins/<name>.sprx` (persistent) |
| `pkg-install <name> ...` | `/dev_hdd0/packages/<name>.pkg` (staged), then extracted into `/dev_hdd0/game/<TITLE_ID>/` |

`vsh-plugin-unload <name>` will best-effort delete `/dev_hdd0/tmp/sdb/<name>.sprx`
if it exists (cleanup of transient uploads). `/plugins/` files are never
touched by `unload` — those are owned by `install` / `uninstall`.

### Self-replace

To update `simple-debug-bridge` itself, FTP the new `.sprx` over the existing
one and run `restart-xmb`. SDB cannot stop/unload itself in-process — the
thread issuing the stop is the one whose code pages get unmapped.

## Layout

```
simple-debug-bridge/
├── src/
│   └── prx.c               # plugin entry — _start / _stop only
├── include/
│   ├── server.h            # accept loop, command dispatch, teardown
│   ├── cmd-common.h        # shared reply / parse helpers used by every cmd-*
│   ├── cmd-introspect.h    # module-list, process-list, process-info
│   ├── cmd-file.h          # pull-file / push-file / delete-file / list-dir
│   ├── cmd-stat-tree.h     # recursive sha1'd snapshot for install diffs
│   ├── cmd-capture.h       # capture + display-info (vsh-side framebuffer read)
│   ├── cmd-trace.h         # module-trace-on / module-trace-off
│   ├── cmd-read-mem.h      # read-mem (raw vsh address-space dump)
│   ├── fileio.h            # socket-coupled file streaming (recvFile / sendFileWindow)
│   ├── capture.h           # vsh-side framebuffer + RSX FIFO helpers
│   ├── plugin.h            # vsh plugin install/uninstall + boot_plugins.txt edits
│   └── pkg.h               # debug-format .pkg parser + extractor (sha1 keystream cipher)
├── simple-debug-bridge.vcxproj
└── README.md
```

Layering:

```
  server.h       ← wire protocol, command dispatch
     ├── fileio.h        socket«fs streaming (recvFile, sendFileWindow)
     ├── plugin.h        installPlugin / uninstallPlugin
     │                  └─ wraps fileio.recvFile + file.deleteFile + manifest edit
     └── pkg.h           installPkg / uninstallPkg / extractPkg
                        └─ self-contained debug-pkg parser (header, sha1 cipher, file table, sfo)
```

Generic (non-streaming) fs primitives — `readFile`, `writeFile`, `fileExists`,
`makeDir`, `deleteFile` — live in `libs/simple-lib-plugin/include/file.h` and
are shared across plugins.

Syscall wrappers (`sysPower`, `prxFinalizeSelf`) live in
`libs/simple-lib-plugin/include/syscall.h`.

## Teardown

`_stop` calls `serverStop()` (which signals the accept loop, wakes
`accept()` via `shutdown()`, and joins the thread) followed by
`prxFinalizeSelf()`. Only relevant when something calls
`sys_prx_stop_module` on the PRX (hot reload during dev). On
power-off / reboot LV2 tears everything down externally.

## Port

`8785` — derived from `8000 + (fnv1a32("simple-debug-bridge") % 1000)`.

## Testing

```
echo "ping" | nc <ps3-ip> 8785
echo "restart-xmb" | nc <ps3-ip> 8785
```

Via the local HTTP bridge (with `debug-bridge-client` running):

```
curl http://localhost:8786/ping
curl "http://localhost:8786/pull-file?path=/dev_hdd0/tmp/dbg.txt&text=1"
curl "http://localhost:8786/pull-file?path=/dev_hdd0/boot_plugins.txt&text=1"
curl "http://localhost:8786/delete-file?path=/dev_hdd0/tmp/old.txt"
```
