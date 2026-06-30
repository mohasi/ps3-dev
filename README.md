# ps3-dev

Small collection of PS3 Cobra/EVILNAT VSH plugins and apps, built with Sony's official SDK on FW 4.75.

## Layout

```
ps3-dev/
├── common/
│   ├── ps3.targets             shared MSBuild (warnings-as-errors, -O2, clean)
│   ├── npdrm.targets           shared NPDRM packaging post-build for apps
│   └── prx.targets             shared PRX signing post-build for plugins
├── libs/
│   ├── simple-lib-core/        cross-context primitives (printf, dbg, file, VFS + exFAT, ftp, thread, wire, log-backlog, bridge-client)
│   ├── simple-lib-app/         static library for apps (gfx, font, audio, pad, screen, anim, ui)
│   ├── simple-lib-plugin/     header-only PRX-only extras (syscall, vsh)
│   ├── libvshtask_export_stub.a
│   └── libvshmain_export_stub.a
├── apps/
│   ├── app-sample/             demo app showcasing engine features
│   └── file-manager/           PS3 file browser with sprite-based UI
├── plugins/
│   ├── simple-debug-bridge/  remote debug/control over TCP (port 8785)
│   ├── simple-disc-mount/    mounts ISOs from an XMB submenu
│   └── simple-ftp/           anonymous, binary-only FTP server on port 21
├── tools/
│   ├── sprite-packer/          packs sprite PNGs into atlas + C header
│   ├── xml-to-sfo/             generates PARAM.SFO from XML
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
with a built-in hand-written **exFAT** reader/writer backend for removable USB
(`exfat` + the shared `usb-storage` device layer) that the file helpers (`file`)
route through, the shared anonymous **FTP** server (`ftp`), PPU thread spawn
(`thread`), string utilities, framed TCP protocol (`wire`), pre-connect log ring
(`log-backlog`), and the producer-side bridge client (`bridge-client`). Built
with `-fno-builtin-printf
-nodefaultlibs` so it links safely into `vsh.self`. Must appear
**before** SDK stubs in the link order or the PRX silently fails to
load.

### simple-lib-app
Reusable static library for apps. Provides 2D rendering, font, audio,
input, screen lifecycle, animation, color palette, and UI components
(label, breadcrumb, image, slice, checkbox, circle, line, rectangle,
triangle). Apps link `libsimple-lib-app.a` and include headers from
`libs/simple-lib-app/include/`. Built on top of `simple-lib-core`.

### simple-lib-plugin
Header-only library for the PRX-only extras: LV2 syscall trampolines
(`syscall.h`) and VSH NID stubs (`vsh.h`). No archive output — plugins
just add its `include/` path. All non-PRX-specific helpers live in
`simple-lib-core`.

## Plugins

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

### file-manager
PS3 file browser with sprite-based UI. Features directory listing with file-type
icons, checkboxes, breadcrumb navigation, hold-to-scroll, image/audio viewers,
read/write access to **exFAT and NTFS USB drives** (via the shared VFS, with hotplug), and
a sprite atlas generated at build time via `sprite-packer`. See
`apps/file-manager/README.md` for details.

## Tools

### sprite-packer
Packs a directory of PNGs into a power-of-2 atlas and generates a C header
(`sprite-regions.h`) with named `SpriteRegion` coordinates. Used by file-manager's
pre-build step.

### xml-to-sfo
Generates `PARAM.SFO` from a human-readable XML source. Used by app pre-build steps.

