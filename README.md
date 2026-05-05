# ps3-dev

Small collection of PS3 Cobra/EVILNAT VSH plugins and apps, built with Sony's official SDK on FW 4.75.

## Layout

```
ps3-dev/
├── libs/
│   ├── simple-ps3-lib/         static library for apps (gfx, font, audio, pad, screen, anim)
│   ├── simple-ps3-prx-lib/    static library for VSH plugins (printf, dbg, file, string-utilities, vsh)
│   ├── libvshtask_export_stub.a
│   └── libvshmain_export_stub.a
├── apps/
│   └── app-sample/             demo app showcasing engine features
├── plugins/
│   ├── simple-disc-mount/      mounts ISOs from an XMB submenu
│   └── simple-ftp/             anonymous, binary-only FTP server on port 21
├── tools/scetool/              PRX signing tool + keys, invoked by PostBuildEvent
├── out/                        build outputs (.sprx plugins, .pkg apps)
└── README.md
```

## Libraries

### simple-ps3-lib
Reusable static library for apps. Provides 2D rendering, font, audio, input,
screen lifecycle, animation, color palette, and file utilities. Apps link
`libsimple-ps3-lib.a` and include headers from `libs/simple-ps3-lib/include/`.

### simple-ps3-prx-lib
Static library for VSH plugins (PRX context). Compiled with `-fno-builtin-printf
-nodefaultlibs` for safe use inside `vsh.self`. Provides a drop-in printf
replacement, debug logging, file helpers, string utilities, and VSH exports.
Plugins link `libsimple-ps3-prx-lib.a` — it must appear **before** SDK stubs
in the link order or the PRX silently fails to load.

## Plugins

### simple-disc-mount
Adds a "Mount Disc Image" submenu below "Package Manager" in the XMB Games
column, listing every `.iso` in `/dev_hdd0/PS3ISO`. Selecting an item mounts
it as a virtual Blu-ray disc via Cobra syscalls. Uses Sony's `webrender_plugin`
as an entry point to trigger the mount from the XMB menu. See
`plugins/simple-disc-mount/README.md` for details.

### simple-ftp
Minimal FTP server: PASV-only, binary-only, anonymous. Listens on :21 once
XMB is ready. Up to two concurrent sessions. Full filesystem access. See
`plugins/simple-ftp/README.md` for details.

## Apps

### app-sample
Demo app showcasing the engine features: RSX 2D renderer, system fonts, audio,
animation, input, and screen/overlay lifecycle. See
`apps/app-sample/README.md` for details.
