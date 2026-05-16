# debug-bridge-client

WPF companion app for the `simple-debug-bridge` PS3 plugin.

## features

- persistent duplex TCP socket to the PS3 plugin (port 8785); background
  auto-reconnect on drop
- **Commands** menu — Screenshot, List VSH Plugins, Restart XMB, Restart PS3, Shutdown
- **Plugins** menu — Install / Uninstall VSH plugin (`.sprx` upload)
- **Files** menu — Get / Save / Delete file on the PS3
- **Screen** tab — renders captures live as they come back from the bridge
- **Log** tab — connection events, sent commands, framed PS3 replies
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
├── Ps3Connection.cs             # tcp transport + auto-connect probe
├── HttpBridge.cs                # local http proxy
├── App.config
└── debug-bridge-client.csproj
```

## build

requires .NET Framework 3.5. builds with MSBuild / Visual Studio.
