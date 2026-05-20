# debug-bridge-client

WPF companion app for the `simple-debug-bridge` PS3 plugin.

## features

- persistent duplex TCP socket to the PS3 plugin (port 8785); background
  auto-reconnect on drop
- **Commands** menu — Screenshot, Restart XMB, Restart PS3, Shutdown
- **Plugins** menu — Install / Uninstall VSH plugin (`.sprx` upload)
- **Files** menu — Get / Save / Delete file on the PS3
- **View** menu — Clear (wipes Activity, Logs, and Screen for a clean test run)
- **Activity** tab — host-side events: connection state, sent commands, framed replies
- **Logs** tab — live PS3-side log lines forwarded by the bridge (LOG frames from
  every registered producer plugin/app — `[sdb]`, `[sdm]`, `[ftp]`, ...)
- **Screen** tab — renders captures live as they come back from the bridge
- **Modules** tab — process-first tree over `process-list` → `module-list` →
  `module-info`. Top-level nodes are processes the bridge can see (always
  `vsh`, plus any registered app); expanding a process lists its loaded PRXs,
  and expanding a module lazily fetches its ELF segments, linkage tables,
  exports and imports. Refresh re-enumerates from the top.
- status bar — connection indicator (bottom right) + HTTP bridge URL
- local HTTP proxy on `http://localhost:8786/` — forwards commands to the PS3

## http api

| endpoint | description |
|---|---|
| `GET /` | usage help |
| `GET /status` | returns `connected` or `disconnected` (local only) |
| `GET /<command>` | forwards `<command>` to the PS3, returns response |
| `GET /<command>?<args>` | forwards `<command> <args>` to the PS3 |

Examples: `/ping`, `/restart-xmb`, `/restart-ps3`, `/shutdown`.

## configuration

PS3 IP is read from `App.config` key `Ps3IpAddress`. Falls back to
`Ps3Connection.DefaultHost` (`10.0.0.2`) if unset.

## layout

```
debug-bridge-client/
├── App.xaml / App.xaml.cs       # WPF app entry, theme merge
├── Theme.xaml                   # dark theme resources
├── MainWindow.xaml(.cs)         # UI shell, menu, status, log
├── ModulesView.xaml(.cs)        # Modules tab: process → module → details tree
├── Modules.cs                   # ModuleSource + ProcessEnumerator + parsers
├── Ps3Connection.cs             # tcp transport + auto-connect probe
├── Ps3Reply.cs                  # framed reply value type
├── HttpBridge.cs                # local http proxy
├── App.config
└── debug-bridge-client.csproj
```

## build

requires .NET Framework 3.5. builds with MSBuild / Visual Studio.
