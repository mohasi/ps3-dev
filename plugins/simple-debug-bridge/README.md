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
| `screenshot [x,y,w,h]` | 🔲 | capture framebuffer (full or region) |
| `terminate` | 🔲 | kill running game process |
| `launch <titleid>` | 🔲 | launch installed title |
| `pixel <x,y>` | 🔲 | read single pixel RGBA |
| `input <buttons>` | 🔲 | send fake pad input |
| `vsh-plugin-list` | ✅ | list every PRX loaded into vsh.self via `sys_prx_get_module_list` (syscall 494) + `sys_prx_get_module_info` (syscall 495). One line per module: `<id>\t<name>\t<filename>`, then `OK <count>`. Includes system modules — useful for debugging. |
| `vsh-plugin-load <name> <size>` | 🔲 | upload `<size>` bytes to `/dev_hdd0/tmp/sdb/<name>.sprx`, then load + start. Replaces if already loaded by same name. |
| `vsh-plugin-load <ps3-path>` | 🔲 | load + start a VSH PRX already on disk (no upload). Path may be quoted (`"..."`) to be unambiguous. |
| `vsh-plugin-unload <name>` | 🔲 | stop + unload a VSH PRX by module name |
| `vsh-plugin-enable <name>` | 🔲 | ensure exactly one active line for `<name>` in `boot_plugins.txt` (canonical path `/dev_hdd0/plugins/<name>.sprx`) |
| `vsh-plugin-disable <name>` | 🔲 | ensure exactly one commented line for `<name>` in `boot_plugins.txt` |
| `vsh-plugin-install <name> <size>` | ✅ | upload `<size>` bytes to `/dev_hdd0/plugins/<name>.sprx`, then `enable`. Thin wrapper over `save-file` + `setPluginLine(ACTIVE)`. |
| `vsh-plugin-uninstall <name>` | ✅ | remove every line referencing `<name>` from `boot_plugins.txt`, then `delete-file /dev_hdd0/plugins/<name>.sprx`. Thin wrapper over `setPluginLine(ABSENT)` + `delete-file`. |
| `get-file <path> [offset] [length]` | ✅ | stream raw bytes back. Response is `<n>\n<n bytes>` on success, `ERR ...\n` on failure. `length=0` (or omitted) means "to end of file". |
| `save-file <path> <size>` | ✅ | upload `<size>` bytes to `<path>` (truncating). Parent directory must already exist — use a separate `mkdir`-equivalent if needed. |
| `delete-file <path>` | ✅ | unlink `<path>`. Idempotent: missing file returns `OK`. |

## Protocol

Text-based, one command per fresh TCP connection:

```
request:  <command> [args...]\n
response: <status> [message]\n
```

Status: `OK` or `ERR <message>`.

Some commands (e.g. `vsh-plugin-list`) return zero or more body lines
*before* the final status line. The status line is always the terminator —
clients read lines until one starts with `OK` or `ERR`.

### Binary upload framing

Commands that ship a binary payload (e.g. `vsh-plugin-load <name> <size>`,
`vsh-plugin-install <name> <size>`) end their command line with a byte count,
then send exactly that many raw bytes immediately after the `\n`:

```
request:  <command> <args...> <byte-count>\n
request:  <byte-count raw bytes>
response: <status> [message]\n
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
│   ├── fileio.h            # socket-coupled file streaming (recvFile / sendFileWindow)
│   └── installer.h         # plugin install/uninstall — wrappers over fileio + file.h
├── simple-debug-bridge.vcxproj
└── README.md
```

Layering:

```
  server.h       ← wire protocol, command dispatch
     ├── fileio.h        socket«fs streaming (recvFile, sendFileWindow)
     └── installer.h     pluginInstall / pluginUninstall
                       └─ wraps fileio.recvFile + file.deleteFile + manifest edit
```

Generic (non-streaming) fs primitives — `readFile`, `writeFile`, `fileExists`,
`makeDir`, `deleteFile` — live in `libs/simple-ps3-prx-lib/include/file.h` and
are shared across plugins.

Syscall wrappers (`sysPower`, `prxFinalizeSelf`) live in
`libs/simple-ps3-prx-lib/include/syscall.h`.

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
curl "http://localhost:8786/get-file?path=/dev_hdd0/tmp/dbg.txt&text=1"
curl "http://localhost:8786/get-file?path=/dev_hdd0/boot_plugins.txt&text=1"
curl "http://localhost:8786/delete-file?path=/dev_hdd0/tmp/old.txt"
```
