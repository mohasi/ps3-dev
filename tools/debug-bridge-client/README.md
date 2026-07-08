# debug-bridge-client

WPF companion app for the `simple-debug-bridge` PS3 plugin.

## features

- persistent duplex TCP socket to the PS3 plugin (port 8785); background
  auto-reconnect on drop
- **Commands** menu — Screenshot, Stat Tree, Restart XMB, Restart PS3, Shutdown
- **Files** menu — Pull / Push / Delete file on the PS3 (Pull has presets
  for `stat-tree.txt`, `dbg.txt`, `trace-capture.bin` plus a Custom... prompt;
  pulled files are written to disk via Save dialog)
- **Packages** menu — Install package (`.pkg` upload + extract)
- **Plugins** menu — Install / Uninstall VSH plugin (`.sprx` upload)
- **View** menu — Clear (wipes Activity, Logs, and Screen for a clean test run)
- **Activity** tab — host-side events: connection state, sent commands, framed replies
- **Logs** tab — live PS3-side log lines forwarded by the bridge (LOG frames from
  every registered producer plugin/app — `[sdb]`, `[sdm]`, `[ftp]`, ...)
- **Screen** tab — renders captures live as they come back from the bridge
- **Modules** tab — process-first tree over `process-list` → `process-info` →
  `module-info`. Top-level nodes are processes the bridge can see (always
  `vsh`, plus any registered app); expanding a process lists its loaded PRXs,
  and expanding a module lazily fetches its ELF segments, linkage tables,
  exports and imports. Refresh re-enumerates from the top.
- **Trace** tab — arm `module-trace-on` on the bridge, exercise the system,
  stop to pull the capture and fill a filterable grid (one row per outgoing
  import call).
- status bar — connection indicator (bottom right) + HTTP bridge URL
- local HTTP proxy on `http://localhost:8786/` — forwards commands to the PS3

## http api

The proxy serves both well-known endpoints with typed query params and a
generic `/<command>` fallback that forwards positional args.

| endpoint | description |
|---|---|
| `GET /` | usage help |
| `GET /status` | returns `connected` or `disconnected` (local only, never blocks on the ps3) |
| `GET /pull-file?path=<p>[&offset=N&length=N][&text=1]` | stream file bytes back (`application/octet-stream`, or `text/plain` with `text=1`) |
| `GET /delete-file?path=<p>` | unlink `<p>` on the ps3 |
| `GET /list-dir?path=<p>` | tab-separated `<kind>\t<size>\t<mtime>\t<name>` lines |
| `GET /capture?x=X&y=Y&w=W&h=H` | raw ARGB8888 framebuffer region (vsh only); also mirrored to the Screen tab |
| `GET /read-mem?<hexAddr>&<decLen>` | raw bytes from vsh address space (positional args) |
| `GET /stat-tree?root=<p>` | trigger recursive sha1'd snapshot on the ps3; reply is `OK files=<n> dirs=<n> -> /dev_hdd0/tmp/stat-tree.txt`. Long-running (~40 s on `/dev_hdd0`); use a request timeout of at least 5 minutes. Pull the output file separately with `/pull-file`. |
| `GET /<command>[?arg1&arg2&...]` | generic passthrough: forwards `<command> arg1 arg2 ...` and returns the text reply |
| `POST /<command>?<args>` | same, but the request body becomes the upload payload (`<args> <byte-count>` is appended to the command line) |

Examples: `/ping`, `/restart-xmb`, `/restart-ps3`, `/shutdown`.

## configuration

PS3 IPs are read from `App.config` key `Ps3IpAddresses`, a comma-separated list
(default `10.0.0.2` if unset). All listed hosts are dialed in parallel on every
reconnect; the first to answer becomes the active connection. Add more addresses
to the list to cover extra interfaces (e.g. wifi).

## layout

```
debug-bridge-client/
├── App.xaml / App.xaml.cs       # WPF app entry, theme merge
├── Theme.xaml                   # dark theme resources
├── MainWindow.xaml(.cs)         # UI shell, menu, tabs, log pipeline
├── ModulesView.xaml(.cs)        # Modules tab: process → module → details tree
├── Modules.cs                   # ModuleSource + ProcessEnumerator + parsers
├── TraceView.xaml(.cs)          # Trace tab: arm/stop module-trace, filterable grid
├── TraceCapture.cs              # parser for /dev_hdd0/tmp/trace-capture.bin frames
├── TraceHeatmap.cs              # per-module call heatmap colouring for the grid
├── NidNames.cs / NidProtos.cs   # offline NID → name / prototype lookup
├── NidJson.cs                   # shared NID-json file finder + key parser
├── Ps3Connection.cs             # tcp transport + auto-connect probe
├── Ps3Reply.cs                  # framed reply value type
├── HttpBridge.cs                # local http proxy (port 8786)
├── App.config                   # PS3 IP override
└── debug-bridge-client.csproj
```

## build

requires .NET Framework 4.0. builds with MSBuild / Visual Studio.
