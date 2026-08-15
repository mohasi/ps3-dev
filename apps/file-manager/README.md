# file-manager

PS3 homebrew file browser with a flat, themeable UI. Sony official SDK 4.75, targets EVILNAT 4.93 CFW.

## Features

- **Directory browsing** — navigates the full filesystem starting from `/`, with exFAT and NTFS USB drives mounted alongside the HDD
- **Removable USB (exFAT + NTFS)** — exFAT- and NTFS-formatted USB drives, which the PS3 firmware itself can't read, are mounted by built-in hand-written drivers and appear at the root as `/exfat0`, `/exfat1`, … and `/ntfs0`, `/ntfs1`, … beside the cellFs devices. Fully read/write (browse, copy, move, delete, rename, new file/folder), with superfloppy, MBR- and GPT-partitioned sticks supported. Insertion and ejection are detected automatically (hotplug); removable volumes are marked with a USB badge on the folder icon, and pulling a stick you're inside drops you back to root
- **File-type icons** — classifies files into 12 types (folder, text, audio, video, image, executable, compressed, disc ISO, package, document, database, generic) with a per-type glyph from the icon font
- **File metadata columns** — rows show `Name | Type | Size | Modified | Permissions`, with Modified displayed in local time as `DD/MM/YY HH:MM`
- **Breadcrumb navigation** — path-driven breadcrumb bar, rebuilt on directory change
- **Selection** — per-row checkboxes with a checked counter. Tap square toggles the focused row; hold square (≥ 400 ms) checks all rows in the directory, or unchecks if all are already checked.
- **Folder sizes** — folder rows show recursively-totalled size alongside files
- **Sorting** — L1 cycles through Name, Size and Modified, each ascending then descending. Folders always come before files and the name breaks ties. Name, Size and Modified each carry a mark in their heading; the column being sorted on shows the direction. The chosen order is saved to `settings.txt` as `sort=` and restored on the next launch.
- **Hold-to-scroll** — press-and-hold D-pad for continuous scrolling with repeat delay
- **Search** — Start opens a search: type a query on the on-screen keyboard and it walks the current folder tree on a background worker, showing a live "N found" count in the standard progress dialog. Results are their own list (same columns as the file list) that you can act on or jump into.
- **Sidepanel** — triangle opens a slide-in action menu (copy, cut, paste, delete, rename, new file/dir, edit, zip/unzip, dump disc, properties) with a header summarizing the current selection (single file, single folder with recursive file count, or multi-selection totals)
- **File operations** — copy / cut / paste (move), delete, rename, and create new file / folder, all via the on-screen keyboard where a name is needed. Name collisions are resolved with a consistent merge/replace/keep model (see below); after a paste the cursor lands on the topmost pasted item.
- **Image viewer** — full-screen viewer for PNG and JPEG images. Opens when X is pressed on a supported image file. Features L1/R1 navigation through sibling images in the directory, L2/R2 zoom (center-pinned, 10%–500%), D-pad pan, and Circle to close. Images decode asynchronously on a background worker (no UI freeze), with a single-image VRAM footprint (the previous image is freed before each load). Oversized images are rejected with a persistent error caption; VRAM upload failures show "(out of VRAM)".
- **Audio player** — full-screen player for WAV, OGG, MP3 and FLAC. Opens when X is pressed on a supported audio file (loaded on a background worker so the overlay appears instantly with a "Loading…" note). Shows the file icon, filename, and track title (from ID3 / Vorbis comment) as a subtitle, a live waveform, a seek bar with elapsed/total/remaining times, and a left-edge volume meter. X toggles play/pause, D-pad left/right seeks (single tap ≈ 1s, hold ramps up; audio mutes while scrubbing), up/down adjusts volume, Circle closes.
- **Video player** — full-screen player for H.264 video with AAC/PCM audio (via `simple-lib-av`), covering MKV and MP4 (including fragmented MP4). Opens with X on a supported video file, with seek and audio.
- **Text editor** — opens text files in an editable full-screen overlay with its own on-screen keyboard; edit, save, exit.
- **Hex viewer** — opens any file as a paged hex dump, streamed from disk so large files don't load into RAM.
- **Properties** — the sidepanel's "Properties" opens a details panel for the highlighted file: name, type, size (human-readable and exact bytes), modified time, permissions, location, and its SHA-1. The hash is computed on a background worker so the panel opens instantly and shows "Calculating… N%" until it lands; closing the panel stops the work.
- **Disc dump** — with a disc in the drive, the sidepanel at the root offers "Dump Disc": a sector-for-sector copy of the whole disc to `/dev_hdd0/dumps/<TITLE_ID>.iso`, with free-space check up front, live progress and cancel. The image is exactly what the drive returns, so a PS3 game disc lands encrypted (decrypting it needs the disc keys, which is a separate job) while PS2 and data discs land readable. Unreadable sectors are retried, then zero-filled and reported rather than silently skipped; a cancelled or failed dump removes its partial file.
- **Disc mount** — X on a disc image (`.iso`) mounts it as the Blu-ray disc via Cobra, so the game shows up in the XMB Game column and boots as if the disc were in the drive; the footer button reads "Mount" instead of "Open" on those rows and a short message at the bottom of the screen says whether it worked. The image must sit on the internal drive or a FAT32 USB stick — Cobra reads it with its own kernel-side code and can't see the app's NTFS/exFAT or network volumes. The mount is remembered, so the `simple-disc-mount` plugin puts it back after a reboot, and it shares that plugin's mounting code from `simple-lib-core`.
- **FTP server** — Select toggles a built-in FTP server on the console (from `simple-lib-core`); the button label shows the console's address and port while it's running.
- **Google Drive** — your Drive appears at the root as a `Google Drive` folder (folder icon with a Google badge) and works like any other folder: browse, open, copy in and out, create folders, rename and delete. Uploads stream in 1 MB pieces, so file size isn't limited by memory, and deleting moves the item to Drive's own trash rather than destroying it. Google Docs and Sheets are listed but can't be copied — they aren't files, they live on Google's servers. The folder only appears once you've set it up (below) — leave the settings empty and there's no Drive folder at all — and even then it costs nothing until you open it, since the console only signs in on the first entry. A search started from the device list skips it (searching your whole Drive over the internet would take forever); start the search inside the folder to search Drive.
- **Native button glyphs** — the footer and dialog button hints render the PS3's own XMB button art, decoded at runtime from the system imagefont via app-lib's `console-glyphs`, so no button sprites are shipped

## Theming

The whole UI is flat/Metro: a solid-colour background with panels, highlights, separators and the on-screen keyboard all drawn in code as square-cornered boxes. Icons are font glyphs tinted to the theme. Because everything is code-drawn from a live palette, colours can change at runtime.

- **Built-in themes** — `Original Blue` (default), `Light` and `Dark`.
- **Switching** — press **R1** on the main screen to cycle themes instantly; the footer shows an **R1 Theme** hint. Your choice is saved and restored on the next launch.
- **Two theme files** — the shipped defaults live in `res/themes.txt`. On first launch that file is copied to `/dev_hdd0/tmp/file-manager/themes.txt`, which is the one you can edit. Themes are keyed by name: change a colour under a built-in name and it overrides ours; delete a block (or the whole file) and it falls back to the shipped `res` version; add a new named block and it appears in the R1 cycle. The chosen theme name is stored in `/dev_hdd0/tmp/file-manager/settings.txt`.
- **Colour format** — `#RRGGBB` for opaque, `#RRGGBBAA` when a colour needs transparency (alpha is last). Any field a theme omits inherits from Original Blue.

Known gap: on the Light theme the side panel and its icons are still light-on-light (low contrast); it needs dark icon variants, tracked for later.

## Google Drive setup

Browsing your whole Drive needs Google's full `drive` permission, which is only granted through a real
browser sign-in — and the console has no browser. So you approve it **once on a PC** and paste the
result into the console's settings file; after that the console signs itself in on its own.

1. At https://console.cloud.google.com create (or reuse) a project, and under **APIs & Services →
   Library** enable the **Google Drive API**.
2. Under **OAuth consent screen**, pick user type *External* and add your own Google account as a
   *test user*. (Leaving the app in Testing is fine, but its sign-in expires after 7 days — publish
   the app to remove that.)
3. Under **Credentials → Create credentials → OAuth client ID**, choose application type
   **Desktop app**. Copy the client ID and client secret.
4. On the PC run `dev/tools/get-gdrive-token.ps1`. Your browser opens, you approve Drive access, and
   the script prints three `google_…=` lines.
5. Paste those three lines into `/dev_hdd0/tmp/file-manager/settings.txt` on the console (over FTP),
   then relaunch the file manager and open the **Google Drive** folder.

On the first successful connect the console encrypts all three values into one `google_auth_enc=` line
tied to *this* console and deletes the plaintext ones, so a copied settings.txt is useless elsewhere.

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

## Controls

- **D-pad** — move / hold to scroll
- **X** — enter folder / open file in its viewer
- **Circle** — go up a folder / close an overlay
- **Square** — toggle row checkbox (hold to select all)
- **Triangle** — Options (sidepanel)
- **L1** — cycle sort order
- **R1** — cycle theme
- **Start** — Search
- **Select** — toggle FTP server

## Architecture

All filesystem access goes through the `simple-lib-core` **VFS** (`vfs.h` / `file.h`), a single `/`-rooted namespace over the cellFs devices (HDD, FAT32 USB) and the built-in exFAT/NTFS backends — so the browser, copy/move/delete workers and the folder-sizer never special-case a filesystem. `main.c` brings the VFS up (`initVfs`, which registers the exFAT and NTFS backends and starts the VFS-owned hotplug poll thread) and loads the theme palettes (`initThemes`, before any screen reads the active theme); the app registers a mounts-changed callback so the root listing refreshes when the poll thread sees an inserted/ejected USB volume.

`theme.c` owns the palette system: a `Theme` is a set of named colours, and the app draws every piece of chrome from the currently-active one. The widgets and overlays in `simple-lib-app` stay theme-agnostic — colours are passed to them as parameters, and the app re-pushes the palette (`applyThemeToHome`) when R1 switches theme. Chrome drawn live each frame re-themes for free; pre-rendered text labels bake their colour when created, so a live switch recolours them explicitly via `setLabelColor` / the per-widget `retheme…` hooks.

`home.c` is the screen orchestrator: it owns shared resources (font, click/check sfx), wires the widgets and overlays, and routes input. Widgets (`file-list`, `search-list`, `clock-widget`, `free-space-widget`, `footer-widget`) and overlays (`sidepanel`, `confirm-overlay`, `progress-overlay`, `image-viewer-overlay`, `audio-player-overlay`, `video-player-overlay`, `text-editor-overlay`, `hex-viewer-overlay`) borrow those resources via init functions and never own them.

`selection-actions.h` is the shared vocabulary between `file-list` (which produces the selection summary and the available action list) and `sidepanel` (which presents them). `file-actions.c` is a thin dispatch layer: for create/rename it just opens the on-screen keyboard and forwards the result, while `file-list` owns the filesystem work, name validation and all conflict prompts. The actual byte-moving (copy/move/merge/delete) runs on the background `file-task` worker behind `progress-overlay`; `paste.c` and `delete.c` are those task bodies. `confirm-overlay` provides the modal prompts — two buttons, or three when a middle (square) option like rename's Merge is needed.

Search runs on its own background worker (`search-controller` / `search-list`): it walks the tree while the shared `progress-overlay` shows an indeterminate "busy" state (title + live count + Cancel) polling the walk for status and completion.

## Layout

```
file-manager/
├── file-manager.vcxproj
├── file-manager.conf       # make_package_npdrm config
├── PARAM.SFO.xml           # human-readable SFO source
├── PARAM.SFO               # generated binary
├── ICON0.PNG               # 320x176
├── include/
│   ├── theme.h             # Theme struct + active-theme accessors
│   ├── selection-actions.h # shared action vocabulary + selection summary
│   ├── file-actions.h
│   ├── search-controller.h
│   ├── clipboard.h         # cut/copy storage
│   ├── paste.h             # move/copy/merge task body + conflict scan
│   ├── delete.h            # delete task body
│   ├── folder-sizer.h      # background recursive sizing
│   ├── file-type.h         # name → file-type classification
│   ├── screens/            # home
│   ├── widgets/            # clock, free-space, file-list, search-list, footer, list-row-chrome
│   └── overlays/           # sidepanel, confirm, progress, image/audio/video viewers, text-editor, hex-viewer, editor-footer
├── src/
│   ├── main.c
│   ├── theme.c             # palette load/override/switch + settings persistence
│   ├── selection-actions.c # action title/subtitle/icon lookup
│   ├── file-actions.c      # action dispatch (keyboard launch + forward)
│   ├── search-controller.c
│   ├── clipboard.c         # cut/copy storage
│   ├── paste.c             # move/copy/merge worker + conflict pre-scan
│   ├── delete.c            # delete worker
│   ├── folder-sizer.c      # background recursive folder sizing
│   ├── file-type.c         # file-type classification
│   ├── screens/home.c
│   ├── widgets/            # clock, free-space, file-list, search-list, footer
│   └── overlays/           # sidepanel, confirm, progress, image/audio/video viewers, text-editor, hex-viewer
├── res/                    # runtime assets (themes.txt, click.wav, check.wav, mono font); the UI background is a solid colour drawn in code, all icons come from the icon font in simple-lib-app
├── bin/<Cfg>/              # OutDir + package staging
└── obj/<Cfg>/              # IntDir
```

## Build pipeline

1. **Pre-build:**
   - `xml-to-sfo` generates `PARAM.SFO` from `PARAM.SFO.xml`
2. **Compile:** Sony GCC PS3 toolset compiles `src/*.c` → `bin/<Cfg>/file-manager.ppu.elf`
3. **Post-build:** NPDRM packaging via shared `common/npdrm.targets` → `.pkg` in `out/`

Builds and deploys go through the ps3 MCP tool (build the VM, deploy over the debug bridge); `res/themes.txt` is copied into the package's USRDIR at build time.

## Icons

All UI icons come from the shared icon font in `simple-lib-app` (see its README) — a single embedded TTF whose glyphs are drawn tinted to the theme colour at any size. There are no per-app image assets: file-type icons, the side-panel action icons, the title folder, the clock and the checkbox are all font glyphs referenced by name (`ICON_*` from `ui/icon-ids.h`). All panel/chrome shapes are drawn in code from the theme palette.

## Identity

- TITLE_ID: `FILEMGR01`
- Content_ID: `HB0001-FILEMGR01_00-FILEMANAGERHB001`

## Dependencies

- **simple-lib-app** — renderer, font, input, screen lifecycle, flat UI components
- **simple-lib-av** — H.264/AAC video playback (video viewer)
- **simple-lib-core** — VFS + exFAT/NTFS backends, file/tree helpers, settings-file parsing, FTP server
- **xml-to-sfo** — build-time PARAM.SFO generation
