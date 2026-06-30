# file-manager

PS3 homebrew file browser with a sprite-based UI. Sony official SDK 4.75, targets EVILNAT 4.93 CFW.

## Features

- **Directory browsing** — navigates the full filesystem starting from `/`, with exFAT and NTFS USB drives mounted alongside the HDD
- **Removable USB (exFAT + NTFS)** — exFAT- and NTFS-formatted USB drives, which the PS3 firmware itself can't read, are mounted by built-in hand-written drivers and appear at the root as `/exfat0`, `/exfat1`, … and `/ntfs0`, `/ntfs1`, … beside the cellFs devices. Fully read/write (browse, copy, move, delete, rename, new file/folder), with superfloppy, MBR- and GPT-partitioned sticks supported. Insertion and ejection are detected automatically (hotplug); removable volumes are marked with a USB badge on the folder icon, and pulling a stick you're inside drops you back to root
- **File-type icons** — classifies files into 12 types (folder, text, audio, video, image, executable, compressed, disc ISO, package, document, database, generic) with per-type sprite icons
- **File metadata columns** — rows show `Type | Size | Modified`, with Modified displayed in local time as `DD/MM/YY HH:MM`
- **Breadcrumb navigation** — path-driven breadcrumb bar with chevron separators, rebuilt on directory change
- **Selection** — per-row checkboxes with checked counter. Tap square toggles the focused row; hold square (≥ 400 ms) checks all rows in the directory, or unchecks if all are already checked.
- **Folder sizes** — folder rows show recursively-totalled size alongside files
- **Hold-to-scroll** — press-and-hold D-pad for continuous scrolling with repeat delay
- **Sidepanel** — triangle opens a slide-in action menu (copy, cut, paste, delete, rename, new file/dir, edit, properties) with a header summarizing the current selection (single file, single folder with recursive file count, or multi-selection totals)
- **File operations** — copy / cut / paste (move), delete, rename, and create new file / folder, all via the on-screen keyboard where a name is needed. Name collisions are resolved with a consistent merge/replace/keep model (see below); after a paste the cursor lands on the topmost pasted item.
- **Image viewer** — full-screen viewer for PNG and JPEG images. Opens when X is pressed on a supported image file. Features L1/R1 navigation through sibling images in the directory, L2/R2 zoom (center-pinned, 10%–500%), D-pad pan, and Circle to close. Images decode asynchronously on a background worker (no UI freeze), with a single-image VRAM footprint (the previous image is freed before each load). Oversized images are rejected with a persistent error caption; VRAM upload failures show "(out of VRAM)".
- **Audio player** — full-screen player for WAV, OGG, MP3 and FLAC. Opens when X is pressed on a supported audio file (loaded on a background worker so the overlay appears instantly with a "Loading…" note). Shows the file icon, filename, and track title (from ID3 / Vorbis comment) as a subtitle, a live waveform, a seek bar with elapsed/total/remaining times, and a left-edge volume meter. X toggles play/pause, D-pad left/right seeks (single tap ≈ 1s, hold ramps up; audio mutes while scrubbing), up/down adjusts volume, Circle closes.
- **Sprite atlas** — all UI sprites packed into a single texture, generated at build time

## File operations & conflict resolution

Image files (`.png`, `.jpg`, `.jpeg`, `.bmp`, `.gif`, `.tga`, `.tiff`) are classified as FILE_TYPE_IMAGE and display an image icon in the file list. Only PNG and JPEG formats can be opened in the viewer; unsupported formats keep the icon but have the X button disabled (no default action).

Audio files are classified as FILE_TYPE_AUDIO; the formats the player can actually decode (`.wav`, `.ogg`, `.mp3`, `.flac`) open with X, while other audio extensions keep the icon but have the X button disabled.

Creating, renaming and pasting all share one merge/conflict model. Names are validated (`isValidFileName`) before any filesystem change, and the only operation that ever deletes a populated folder is an explicit **Replace**.

What each action does when the target **name is free** vs. **already taken**:

| Action | Name free | Collides with a file | Collides with a folder |
| --- | --- | --- | --- |
| **New File** | create empty file | prompt **Replace / Cancel** (Replace truncates) | no-op, just select it (a file can't replace a folder) |
| **New Folder** | create folder | no-op, select it | no-op, select it (an empty folder has nothing to merge) |
| **Rename** | rename in place | prompt **Merge / Replace / Cancel** | prompt **Merge / Replace / Cancel** |
| **Copy → same folder** | — | duplicate as `name (n)` | duplicate as `name (n)` |
| **Copy → other folder** | create | merge (file leaves per prompt) | merge into the folder |
| **Cut → same folder** | no-op | no-op | no-op |
| **Cut → other folder** | move | merge then delete source | merge then delete source |

The rename prompt's three buttons are **✕ Merge / ☐ Replace / ◯ Cancel**. Merge folds two folders together (a file target, where merging is meaningless, falls back to Replace); Replace wipes the target then renames onto it; Cancel does nothing.

When a merge would land on existing files, a single prompt up front decides the policy for the whole operation — shown only when there is actually a collision, with the "All" wording reserved for when more than one is known:

| Files that would be overwritten | Prompt |
| --- | --- |
| none | _(no prompt — proceeds)_ |
| one | **✕ Replace / ◯ Keep** |
| many | **✕ Replace All / ◯ Keep All** |

Conflicts are pre-scanned on the main thread before the background paste worker starts, so the worker is never interrupted for a per-file question.

## Architecture

All filesystem access goes through the `simple-lib-core` **VFS** (`vfs.h` /
`file.h`), a single `/`-rooted namespace over the cellFs devices (HDD, FAT32
USB) and the built-in exFAT backend — so the browser, copy/move/delete workers
and the folder-sizer never special-case a filesystem. `main.c` brings the VFS up
(`initVfs`, which registers the exFAT and NTFS backends and starts the VFS-owned
hotplug poll thread); the app registers a mounts-changed callback so the root
listing refreshes when the poll thread sees an inserted/ejected USB volume.

`home.c` is the screen orchestrator: it owns shared resources (font,
spritesheet, click sfx), wires the widgets and overlays, and routes
input. Widgets (`file-list`, `clock-widget`, `free-space-widget`) and
overlays (`sidepanel`, `confirm-overlay`, `progress-overlay`,
`image-viewer-overlay`, `audio-player-overlay`) borrow those resources via
init functions and never own them.

`selection-actions.h` is the shared vocabulary between `file-list`
(which produces the selection summary and the available action list)
and `sidepanel` (which presents them). `file-actions.c` is a thin
dispatch layer: for create/rename it just opens the on-screen keyboard
and forwards the result, while `file-list` owns the filesystem work,
name validation and all conflict prompts. The actual byte-moving
(copy/move/merge/delete) runs on the background `file-task` worker
behind `progress-overlay`; `paste.c` and `delete.c` are those task
bodies. `confirm-overlay` provides the modal prompts — two buttons, or
three when a middle (square) option like rename's Merge is needed.

The image viewer (`image-viewer-overlay`) decodes PNG/JPEG images
asynchronously via a background worker in `image-loader` (from
`simple-lib-app`). It scans the opened image's directory for sibling
supported images and provides L1/R1 navigation with slide-in animation,
L2/R2 zoom, and D-pad pan. The previous image's VRAM is freed before each
upload, so only a single image is resident at a time. Decode failures
and upload failures display persistent error captions ("(image too
large)", "(out of VRAM)").

The audio player (`audio-player-overlay`) plays through the `simple-lib-app`
audio mixer. Loading runs on a background worker so the overlay opens
instantly; WAV streams from disk while OGG/MP3/FLAC decode on demand from
their compressed bytes, so nothing large is held in RAM. The live waveform
is driven by the mixer's rolling amplitude envelope (`getSfxWaveform`), the
seek bar/times come from the stream's tracked position, and seeking is
muted while held so you don't hear it scrub.

## Layout

```
file-manager/
├── file-manager.vcxproj
├── file-manager.conf       # make_package_npdrm config
├── PARAM.SFO.xml           # human-readable SFO source
├── PARAM.SFO               # generated binary
├── ICON0.PNG               # 320x176
├── sprites/                # source sprite PNGs (auto-packed at build)
├── include/
│   ├── sprite-regions.h    # generated by sprite-packer
│   ├── selection-actions.h # shared action vocabulary + selection summary
│   ├── file-actions.h
│   ├── clipboard.h         # cut/copy storage
│   ├── paste.h             # move/copy/merge task body + conflict scan
│   ├── delete.h            # delete task body
│   ├── folder-sizer.h      # background recursive sizing
│   ├── file-type.h         # name → file-type classification
│   ├── screens/            # home
│   ├── widgets/            # clock, free-space, file-list
│   └── overlays/           # sidepanel, confirm-overlay, progress-overlay, image-viewer-overlay, audio-player-overlay
├── src/
│   ├── main.c
│   ├── selection-actions.c # action title/subtitle/icon lookup
│   ├── file-actions.c      # action dispatch (keyboard launch + forward)
│   ├── clipboard.c         # cut/copy storage
│   ├── paste.c             # move/copy/merge worker + conflict pre-scan
│   ├── delete.c            # delete worker
│   ├── folder-sizer.c      # background recursive folder sizing
│   ├── file-type.c         # file-type classification
│   ├── screens/home.c
│   ├── widgets/            # clock, free-space, file-list
│   └── overlays/           # sidepanel, confirm-overlay, progress-overlay, image-viewer-overlay, audio-player-overlay
├── res/                    # runtime assets (background.png, sprites.png, click.wav)
├── bin/<Cfg>/              # OutDir + package staging
└── obj/<Cfg>/              # IntDir
```

## Build pipeline

1. **Pre-build:**
   - `sprite-packer` packs `sprites/` → `res/sprites.png` + `include/sprite-regions.h`
   - `xml-to-sfo` generates `PARAM.SFO` from `PARAM.SFO.xml`
2. **Compile:** Sony GCC PS3 toolset compiles `src/*.c` → `bin/<Cfg>/file-manager.ppu.elf`
3. **Post-build:** NPDRM packaging via shared `common/npdrm.targets` → `.pkg` in `out/`

## Adding sprites

Drop new `.png` files into `sprites/`. They're automatically included in the project (wildcard glob) and packed into the atlas on next build. Enum names are derived from filenames: `my-icon.png` → `SPRITE_MY_ICON`.

## Identity

- TITLE_ID: `FILEMGR01`
- Content_ID: `HB0001-FILEMGR01_00-FILEMANAGERHB001`

## Dependencies

- **simple-lib-app** — renderer, font, input, screen lifecycle, UI components
- **simple-lib-core** — VFS + exFAT backend, file/tree helpers, FTP server (Select/Start FTP Server)
- **sprite-packer** — build-time atlas generation
- **xml-to-sfo** — build-time PARAM.SFO generation
