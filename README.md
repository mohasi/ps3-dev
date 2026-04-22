# ps3-dev

Small collection ofPS3 Cobra/EVILNAT VSH plugins and apps, built with Sony's official SDK on FW 4.75.

## Layout

```
ps3-dev/
├── common/              shared headers + VSH import stubs
│   ├── dbg.h            dbgLog() — appends to /dev_hdd0/tmp/dbg.txt
│   ├── vsh.h            vshNotify(), isXmbReady(), mountDevBlind() syscall
│   └── lib/             libvshtask_export_stub.a, libvshmain_export_stub.a
├── tools/scetool/       PRX signing tool + keys, invoked by PostBuildEvent
├── simple-disc-mount/   adds "Mount Disc Image" to the XMB Games column
└── simple-ftp/          anonymous, binary-only FTP server on port 21
```

Both plugins pick up `common/` via `$(SolutionDir)common` in the vcxproj's
include path, and link against `$(SolutionDir)common\lib\*.a`.

## Plugins

### simple-disc-mount
Injects a submenu below "Package Manager" that lists every `.iso` in
`/dev_hdd0/PS3ISO`. Activating an item currently shows Sony's stock "Cannot
operate" dialog — real mount wiring will come via a `webrender_plugin`
Action() hook.

### simple-ftp
Minimal FTP server: PASV-only, binary-only, anonymous. Listens on :21 once
XMB is ready. Up to 4 concurrent sessions. Full filesystem access (the
console owner's FS is fair game).
