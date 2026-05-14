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
| `install <path>` | 🔲 | install pkg from ps3 filesystem |
| `pixel <x,y>` | 🔲 | read single pixel RGBA |
| `sprx-load <path>` | 🔲 | load and start a prx module |
| `input <buttons>` | 🔲 | send fake pad input |

Power commands fire the syscall directly. LV2 tears the whole system down,
so there is no need to stop the accept loop or close sockets first (same
situation as a power-button shutdown).

## Protocol

Text-based, one command per fresh TCP connection:

```
request:  <command> [args...]\n
response: <status> [message]\n
```

Status: `OK` or `ERR <message>`.

## Layout

```
simple-debug-bridge/
├── src/
│   └── prx.c               # plugin entry — _start / _stop only
├── include/
│   └── server.h            # accept loop, command dispatch, teardown
├── simple-debug-bridge.vcxproj
└── README.md
```

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
