# ps3-dev

PS3 Cobra/EVILNAT VSH plugins and apps, built with Sony's official SDK on FW 4.75, plus the Windows
tools that build and feed them.

## Layout

```
ps3-dev/
├── common/
│   ├── ps3.targets             shared MSBuild (warnings-as-errors, -O2, clean)
│   ├── npdrm.targets           shared NPDRM packaging post-build for apps
│   └── prx.targets             shared PRX signing post-build for plugins
├── libs/
│   ├── simple-lib-core/        cross-context primitives (printf, dbg, file, VFS + exFAT/NTFS, http, ftp, thread, wire, log-backlog, bridge-client)
│   ├── simple-lib-app/         static library for apps (gfx, font, pad, screen, anim, ui)
│   ├── simple-lib-av/          audio + H.264/AAC video playback (mixer, demux, decode, streaming source)
│   ├── simple-lib-https/       modern TLS (BearSSL) — the HTTPS transport the firmware can't do
│   ├── simple-lib-torrent/     BitTorrent: peers, magnets, trackers, piece checking and storage
│   ├── simple-lib-wireguard/   WireGuard client, with its own TCP and DNS inside the tunnel
│   ├── simple-lib-plugin/     header-only PRX-only extras (vsh NID stubs, module hook/inspect)
│   ├── libvshtask_export_stub.a
│   └── libvshmain_export_stub.a
├── apps/
│   ├── app-sample/             demo app showcasing engine features
│   ├── cell-stream/            plays a live stream of a Windows PC, and sends the pad back
│   ├── file-manager/           PS3 file browser, flat themeable UI (image/audio/video/text/hex viewers)
│   ├── renpy-player/           runs Ren'Py visual novels packaged as .rpk
│   ├── swarm/                  torrent client that speaks its own WireGuard tunnel
│   ├── thermal-bench/          heat, fan and clock test tool, saves every run for comparison
│   └── yo-player/              native YouTube client (streams video + audio directly)
├── plugins/
│   ├── simple-cd-info/       puts album and track names back on the XMB for audio CDs
│   ├── simple-cheat-menu/    in-game cheat overlay, synced with the shared cheat repo
│   ├── simple-debug-bridge/  remote debug/control over TCP (port 8785)
│   ├── simple-disc-mount/    mounts ISOs from an XMB submenu
│   └── simple-ftp/           anonymous, binary-only FTP server on port 21
├── tools/
│   ├── debug-bridge-client/    Windows host app: HTTP proxy to the on-console bridge (Logs, install, mem/file)
│   ├── cell-stream-server/     PC side of cell-stream: captures, encodes, and takes the pad back
│   ├── sprite-packer/          packs sprite PNGs into atlas + C header
│   ├── xml-to-sfo/             generates PARAM.SFO from XML
│   ├── nid-dump/               dumps firmware NIDs for symbol resolution
│   ├── patch-studio/           makes texture patches from textures dumped on the console
│   ├── renpy-to-ps3/           packages Ren'Py visual novels to run on PS3
│   ├── rco-studio/             GUI for editing PS3 XMB resource files (RCOs)
│   ├── theme-studio/           GUI to author PS3 static + dynamic 3D themes (.p3t)
│   ├── pkg-custom/             builds the .pkg during an app build
│   └── scetool/                PRX signing tool + keys
├── out/                        build outputs (.sprx plugins, .pkg apps)
└── README.md
```

## Shared Build Infrastructure

All PS3 vcxproj files import `common/ps3.targets` which provides:
- `-O2` optimization
- `-Wall` (all warnings enabled)
- `-Werror` (warnings as errors)
- Custom clean that wipes `bin/` and `obj/` contents

App projects additionally import `common/npdrm.targets` which handles the full NPDRM packaging pipeline (make_fself_npdrm → copy assets → make_package_npdrm → move the resulting `.pkg` to `out/<projectname>.pkg`). The deterministic filename is what `deploy.ps1` relies on for app installs.

## Build & deploy

All PS3 builds run inside the Windows 7 VM via SSH (`dev/vmbuild.ps1`).
The local Windows host never invokes the Sony SDK directly.

```
dev/vmbuild.ps1 <plugins|apps|libs|tools> <name> [Build|Rebuild|Clean] [Release|Debug]
```

### Build stamp

Every build is stamped with `<commit count>-<commit>`, plus `-dirty` when it
includes edits that were never committed (only changes under `libs/` or the
project's own folder count). `vmbuild.ps1` works it out from git — the VM has
no git — and hands it to msbuild, which passes it to the compiler as
`BUILD_NAME` / `BUILD_STAMP`. Every app and plugin logs it at startup via
`logBuildVersion()`, so any `dbg.txt` says which build produced it, and the
commit says which source. Apps also carry the build number as `APP_VER`, which
the console shows in the game's info screen.

A build started any other way (msbuild or Visual Studio directly) stamps
`unknown`, since nothing traced it to a commit.

`deploy.ps1` is the one-shot "build + install on the live PS3" entry point.
It auto-detects whether `<name>` is a plugin or an app from the folder layout:

```
dev/deploy.ps1 simple-debug-bridge -RestartXmb   # plugin: -RestartXmb when self-replacing
dev/deploy.ps1 file-manager                       # app:    restart-xmb is automatic
dev/deploy.ps1 app-sample -NoClean                # app:    keep existing /dev_hdd0/game/<TID>/
```

Plugins are uploaded as `out/<name>.sprx` via `vsh-plugin-install`. Apps are
uploaded as `out/<name>.pkg` via `pkg-install`, which extracts the package
on-device (no XMB install dialog) into `/dev_hdd0/game/<TITLE_ID>/`. Both
paths require `debug-bridge-client.exe` running on the host and the PS3
bridge plugin reachable.

## Libraries

### simple-lib-core
Cross-context primitives shared by both PRX plugins and apps: drop-in
`printf`, leveled timestamped logging (`dbg`), a `/`-rooted **VFS** (`vfs`)
with built-in hand-written **exFAT** and **NTFS** reader/writer backends for
removable USB (`exfat` / `ntfs` + the shared `usb-storage` device layer) that the
file helpers (`file`) route through, a transport-agnostic **HTTP(S)** client
(`http`, with opt-in firmware-cellHttp or BearSSL backends), the shared anonymous
**FTP** server (`ftp`), PPU thread spawn (`thread`), string utilities, framed TCP
protocol (`wire`), pre-connect log ring (`log-backlog`), and the producer-side
bridge client (`bridge-client`). Built with `-fno-builtin-printf
-nodefaultlibs` so it links safely into `vsh.self`. Must appear
**before** SDK stubs in the link order or the PRX silently fails to
load.

### simple-lib-app
Reusable static library for apps. Provides 2D rendering, font,
input, screen lifecycle, animation, color palette, and UI components
(label, breadcrumb, image, slice, checkbox, circle, line, rectangle,
triangle). Apps link `libsimple-lib-app.a` and include headers from
`libs/simple-lib-app/include/`. Built on top of `simple-lib-core`.

### simple-lib-av
Audio + video playback for apps: a multi-stream audio mixer (WAV / OGG / MP3 /
FLAC) and the H.264/AAC video stack (cellVdec/cellAdec wrappers, MKV + MP4
demuxers including fragmented MP4, a header probe, and a seekable source that
reads from a local file or an `http(s)://` URL). Built on `simple-lib-core`; apps
that draw video also need `simple-lib-app`. See `libs/simple-lib-av/README.md`.

### simple-lib-https
Self-contained modern TLS (BearSSL over raw sockets) that reaches hosts the
firmware's RSA-only TLS can't (ECDSA certs, e.g. Cloudflare). It plugs in as the
**modern transport** behind `simple-lib-core`'s http module: an app calls
`initModernHttp()` and every `fetchHttp` / `openHttpStream` runs over BearSSL. Adds
~80 KB, so it is opt-in and apps-only. See `libs/simple-lib-https/README.md`.

### simple-lib-wireguard
A WireGuard client written from the protocol paper: the handshake, the crypto it
needs (ChaCha20-Poly1305, BLAKE2s, X25519), and a hand-written TCP and DNS on top,
so an app's traffic leaves the console inside the tunnel rather than beside it.
See `libs/simple-lib-wireguard/README.md`.

### simple-lib-torrent
The parts of a BitTorrent client that do not depend on how the traffic gets out:
peers (BEP 3), magnet links (BEP 9/10), UDP trackers (BEP 15), piece checking and
storage. The app lends it a network, which for Swarm means the tunnel.
See `libs/simple-lib-torrent/README.md`.

### simple-lib-plugin
Header-only library for the PRX-only extras: VSH NID stubs (`vsh.h`) and
module hook/inspect helpers (`module-hook.h`, `module-inspect.h`). No archive
output — plugins just add its `include/` path. The LV2 syscall trampolines and
all other non-PRX-specific helpers (printf, dbg, VFS, …) live in `simple-lib-core`.

## Plugins

### simple-cd-info
Puts the album, artist and track names back on the XMB when an audio CD goes in. The service the
console used for that was shut down years ago, so current firmware shows "Track 1, Track 2, …".
The names come from the gnudb database instead. Nothing to configure. See
`plugins/simple-cd-info/README.md` for details.

### simple-cheat-menu
In-game cheat overlay. A short PS press over a running game opens a panel listing that title's
cheats; toggling one patches the game's memory live (cobra syscall-8) and verifies the write
landed. Cheats are downloaded per title from the shared
[game-cheats](https://github.com/mohasi/game-cheats) repo, and players can mark a cheat working or
failed — those votes become a crowd score plus a per-build "will this work on your copy" verdict.
See `plugins/simple-cheat-menu/README.md` for details.

### simple-disc-mount
Adds a "Mount Disc Image" submenu below "Package Manager" in the XMB Games
column, listing every `.iso` in `/dev_hdd0/PS3ISO`. Selecting an item mounts
it as a virtual Blu-ray disc via Cobra syscalls. Uses Sony's `webrender_plugin`
as an entry point to trigger the mount from the XMB menu. See
`plugins/simple-disc-mount/README.md` for details.

### simple-ftp
Minimal FTP server: PASV-only, binary-only, anonymous. Listens on :21 once
XMB is ready. Up to two concurrent sessions. Full filesystem access, including
exFAT USB drives (mounted via the shared VFS and surfaced as `/exfat0` …). See
`plugins/simple-ftp/README.md` for details.

## Apps

### app-sample
Demo app showcasing the engine features: RSX 2D renderer, system fonts, audio,
animation, input, and screen/overlay lifecycle. See
`apps/app-sample/README.md` for details.

### cell-stream
Plays a live stream of a Windows PC on the console: picture and sound down, the pad back up, in the
manner of Steam Remote Play. 720p60 over WiFi with about 25 ms from the encoder to the screen, so
games are playable. The pad arrives at the PC either as a mouse and keyboard or as an Xbox
controller. PC side is `tools/cell-stream-server`. See `apps/cell-stream/README.md` for details.

### file-manager
PS3 file browser with a flat, themeable UI. Features directory listing with file-type
icons, checkboxes, breadcrumb navigation, hold-to-scroll, search, image/audio/video viewers,
a text editor and hex viewer, read/write access to **exFAT and NTFS USB drives** (via the
shared VFS, with hotplug), a built-in FTP server, and a runtime theme system (Original Blue /
Light / Dark, switch with R1, user-editable `themes.txt`). See
`apps/file-manager/README.md` for details.

### renpy-player
Runs Ren'Py visual novels on the console. It reads the `.rpk` bundle that `tools/renpy-to-ps3`
produces and plays the script itself: scenes, choices, saves and history, with the typewriter text
and the game menus the originals have.

### swarm
Torrent client that runs its own VPN: the WireGuard tunnel is spoken by the app, so
trackers and peers see the VPN's address and never the console's. Searches the sites
listed in its sources folder, downloads to the hard disk, and can write over content
before deleting it. See `apps/swarm/README.md` for details.

### thermal-bench
Heat, fan and clock test tool. It shows what the console is doing thermally, puts it under a
controlled load, and saves every run, so two runs can be compared: before against after a repaste,
or stock clocks against an overclock. See `apps/thermal-bench/README.md` for details.

### yo-player
Native YouTube client — browse feeds, search, open channels, and play up to 1080p
with sound, streamed as it plays (no PSN, no ads). Talks to YouTube directly, streams
adaptive H.264/AAC over the http module (BearSSL transport), and skips community
**SponsorBlock** segments. Built on `simple-lib-av` + `simple-lib-app`. See
`apps/yo-player/README.md` for details.

## Tools

### debug-bridge-client
Windows host app that pairs with the on-console `simple-debug-bridge` plugin. Hosts a
local HTTP proxy at `http://localhost:8786` that all deploys and the ps3 MCP bridge tools
talk to, and provides a Logs tab (the source of truth for forwarded console logs),
Install & Launch, and memory/file transfer. See `tools/debug-bridge-client/README.md`.

### cell-stream-server
Windows side of the `cell-stream` app. Captures the desktop, encodes it to H.264 on the graphics
card, sends it to the console over UDP, and turns the pad packets coming back into a virtual Xbox
360 controller (ViGEmBus) or into mouse and keyboard. See `tools/cell-stream-server/README.md`.

### sprite-packer
Two modes. **Sprite mode** packs a directory of PNGs into a power-of-2 atlas and
generates a C header (`sprite-regions.h`) of named `SpriteRegion` coordinates (used
by renpy-player's hand-cursor). **Icons mode** turns a Fontello TTF + `config.json`
into an embedded byte array (`icon-data.c`) and a name→codepoint header
(`icon-ids.h`) — the icon font shared by simple-lib-app and its apps.

### xml-to-sfo
Generates `PARAM.SFO` from a human-readable XML source. Every app build runs it
(see `common/npdrm.targets`), stamping the build number into `APP_VER`, so
`PARAM.SFO` is a build output rather than a committed file.

### nid-dump
Dumps firmware NIDs (symbol IDs) from PS3 modules, for resolving VSH/system symbols.
See `tools/nid-dump/README.md`.

### patch-studio
Windows tool for making PS3 texture patches. Textures are dumped from a running game on the console,
edited on the PC, and sent back as a patch that `simple-cheat-menu` applies in game. Nothing is
injected into the game: textures are swapped in video memory and matched by content, so a patch keeps
working across reboots and reloads. See `tools/patch-studio/README.md`.

### renpy-to-ps3
Dark-theme WPF GUI that converts Ren'Py visual-novel games into a single `.rpk`
bundle for the on-console renpy-player. See `tools/renpy-to-ps3/README.md`.

### rco-studio
Dark-theme WPF GUI for editing PS3 XMB resource files (RCOs). Drives bundled rcomage +
GimConv to dump RCOs to editable png/wav/xml, add or edit resources, and recompile with
byte-exact verify. Non-destructive (pristine copies + revert), with sets and shareable
`.rcopatch` patches for whole-theme workflows, one-click migration of pre-4.89 mods onto
current firmware, and FTP deploy straight to the console's dev_blind. See
`tools/rco-studio/README.md`.

### theme-studio
WPF editor for PS3 themes, including true **dynamic 3D themes** — models, motion and lights scripted
in Sony's PSJS — which Sony's own theme creator never supported. Builds custom backgrounds, icons,
cursors and menu sounds into a `.p3t`, with a live preview that plays the scene as you type,
self-contained one-file projects, worked script examples, and FTP deploy. See
`tools/theme-studio/README.md`.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Vendored third-party components
(BearSSL, Ogg Vorbis) keep their own licenses.

