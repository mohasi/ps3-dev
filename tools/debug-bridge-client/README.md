# debug-bridge-client

Windows desktop app (WPF, .NET Framework 4.0) that talks to the on-console
`simple-debug-bridge` PS3 plugin. It keeps one always-on connection to the PS3,
shows the logs the console sends back, and exposes file / memory / plugin tools.
It also runs a small local web server so the `ps3` MCP tool (and any script) can
drive the PS3 over plain HTTP.

Run it: launch `tools\debug-bridge-client\bin\debug-bridge-client.exe` and leave it open. It connects to the
PS3 on its own and reconnects if the console reboots. The `ps3` MCP `deploy` and
bridge tools all go through this app, so it must be running for them to work.

## how it connects

A background thread keeps one TCP connection open to the plugin on port `8785`.
It dials every candidate PS3 address at once and the first to answer wins, so
detection is near-instant however many are dead. A reader thread splits the
incoming stream into two kinds of message: command replies (OK / ERR, handed
back to whoever sent the command) and LOG lines (pushed to the Logs tab).

## features

- **Commands** menu — Stat Tree, Restart XMB, Restart PS3, Shutdown.
- **Files** menu — Pull / Push / Delete a file on the PS3. Pull and Delete have
  presets for `dbg.txt`, `stat-tree.txt`, `trace-capture.bin` plus a Custom...
  prompt; pulled files are saved to disk through a Save dialog.
- **Packages** menu — Install & Launch a `.pkg` (upload + install on the console).
- **Plugins** menu — Install / Uninstall a VSH plugin (`.sprx` upload). This is
  the transport the MCP `deploy` uses.
- **Logs** tab — live log lines the bridge forwards from every producer plugin
  and app (`[sdb]`, `[sdm]`, `[ftp]`, ...), mixed with host-side events
  (connect / disconnect, sent commands, replies) marked with a `---` prefix.
  Toolbar has a live filter box plus copy and clear buttons. This tab is the
  source of truth for forwarded logs — `dbg.txt` on the console only proves the
  producer ran.
- **Modules** tab — a process-first tree: top-level nodes are the processes the
  bridge can see (always `vsh`, plus any registered app); expand a process to
  list its loaded modules, expand a module to fetch its segments, linkage
  tables, exports and imports on demand. Names and prototypes for the symbol IDs
  are resolved offline from the `nid-dump` JSON files.
- **Trace** tab — arm import-call tracing on the bridge, exercise the console,
  then stop to pull the capture and fill a filterable grid (one row per outgoing
  library call), with per-module heatmap colouring.
- Status bar — connection indicator (bottom right) and the local HTTP URL.

## local HTTP proxy

While running, the app serves `http://localhost:8786/`. It forwards commands to
the PS3 and returns the text reply, so tools on the PC can reach the console
without speaking the raw protocol.

| endpoint | description |
|---|---|
| `GET /` | usage help |
| `GET /status` | `connected` or `disconnected` (answered locally, never blocks on the PS3) |
| `GET /pull-file?path=<p>[&offset=N&length=N][&text=1]` | stream a file back (`application/octet-stream`, or `text/plain` with `text=1`) |
| `GET /delete-file?path=<p>` | delete `<p>` on the PS3 |
| `GET /list-dir?path=<p>` | tab-separated `<kind>\t<size>\t<mtime>\t<name>` lines |
| `GET /read-mem?<hexAddr>&<decLen>` | raw bytes from vsh memory (positional args) |
| `GET /stat-tree?root=<p>` | recursive checksummed snapshot on the PS3; reply is `OK files=<n> dirs=<n> -> /dev_hdd0/tmp/stat-tree.txt`. Slow (~40 s on `/dev_hdd0`) — allow at least a 5-minute request timeout, then pull the output file with `/pull-file`. |
| `GET /<command>[?arg1&arg2&...]` | generic passthrough: forwards `<command> arg1 arg2 ...` and returns the reply |
| `POST /<command>?<args>` | same, but the request body is the upload payload (`<args> <byte-count>` is appended to the command line) |

Examples: `/ping`, `/restart-xmb`, `/restart-ps3`, `/shutdown`.

## configuration

The PS3 addresses to try are the `DefaultHosts` array in `Ps3Connection.cs`
(currently `10.0.0.2` and `192.168.2.35`). All are dialed in parallel on every
reconnect and the first to answer becomes the active connection. Add another
address there to cover an extra interface (e.g. wifi), then rebuild.

## layout

```
debug-bridge-client/
├── App.xaml / App.xaml.cs       # WPF app entry, theme merge
├── Theme.xaml                   # dark theme resources
├── MainWindow.xaml(.cs)         # UI shell, menu, tabs, log pipeline
├── ModulesView.xaml(.cs)        # Modules tab: process -> module -> details tree
├── Modules.cs                   # module/process enumeration + parsers
├── TraceView.xaml(.cs)          # Trace tab: arm/stop tracing, filterable grid
├── TraceCapture.cs              # parser for trace-capture.bin frames
├── TraceHeatmap.cs              # per-module call heatmap colouring
├── NidNames.cs / NidProtos.cs   # offline symbol-id -> name / prototype lookup
├── NidJson.cs                   # shared NID-json file finder + key parser
├── Ps3Connection.cs             # tcp transport + parallel auto-connect
├── Ps3Reply.cs                  # framed reply value type
├── HttpBridge.cs                # local http proxy (port 8786)
└── debug-bridge-client.csproj
```

## build

Requires .NET Framework 4.0. Builds with MSBuild or Visual Studio.
