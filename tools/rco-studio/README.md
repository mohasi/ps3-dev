# rco-studio

A dark-theme Windows GUI for editing PS3 XMB resource files (RCOs). It drives the
rcomage command-line tool and Sony's GimConv so that dumping, editing and recompiling
RCOs is painless — no typing paths, no naming files by hand, everything batched.

Built for the request in psx-place thread 50577: auto-created folders, batch dump/compile,
working PNG→GIM conversion, and per-image GIM format preservation.

## Zero setup

- `rcomage.exe` and GimConv are bundled under `tools/` and copied next to the exe on build.
  Nothing to install or configure.
- Dumps go to `./dumps/<rco-name>/` next to the exe, one folder per RCO, created for you.
- `settings.txt` is written on first run with sensible defaults (see **Settings**).

## The basics

Drop `.rco` files on the window (or use **Add RCOs…**). A whole folder of RCOs, or a
saved `.rcoset`, can be dropped too. Each RCO is dumped to `dumps/<name>/`:

- the XML structure and the raw console resources,
- an editable `.png` for every `.gim` image,
- an editable `.wav` for every `.vag` sound (channels joined into one file),
- the text/XML language files as-is.

Edit any of those, check the rows you want, and hit **Compile**. The app converts
whatever you changed back to the native format and recompiles to `compiled/<name>.rco`,
auto-named, with the original header compression. Every compile is then re-dumped and
compared byte-for-byte against your working dump, so a silent mismatch can't slip through.
Dumps from earlier sessions reappear on startup, showing whether they were compiled.

rcomage's own PNG→GIM conversion crashes (the scene's long-standing pain point), so the
app never uses it — it converts via GimConv itself and compiles with `--no-convgim`.

## Editing

Select a row to browse its files in the preview pane: images as thumbnails, sounds you
can play, and the text/XML documents. **Double-click** opens a file in its Windows default
app (sounds play). **Right-click** a tile for:

- **Edit** — opens the file in the editor set in `settings.txt` (greyed out if none is set).
- **Diff** — opens your configured compare tool (e.g. WinMerge) with the dumped original on
  the left and your edited file on the right. Only enabled once the file actually differs.
- **Revert** — restores the file to exactly how it was when the RCO was dumped. For a file
  you *added* (see below), Revert removes it and its XML entry.

A file you have changed is marked with an amber pencil, on both the tile and its row, so
you can see at a glance which RCOs have uncompiled edits. A `~` badge marks lossy formats
(DXT images and all sounds), which lose a little quality each time they are re-encoded; the
app warns once per session before you open one.

### Editing is non-destructive

Every dump keeps a hidden pristine copy of itself. "Edited" means *the file differs from
that copy*, Revert restores from it, and an untouched resource is always copied back to the
compiled RCO byte-for-byte — only files you actually changed are ever re-encoded. Repeatedly
dumping and recompiling an RCO you didn't touch produces an identical file.

## Adding new images and sounds

Drop a **PNG** onto the preview of a selected RCO to add an image it never had. You pick the
GIM format (default RGBA8888 — full colour, never degrades; the dialog shows what the RCO's
other images use). The app converts it and writes the `<Image>` entry into the XML for you.

Drop a **WAV** (mono or stereo, 16-bit PCM) to add a sound — only onto an RCO that already
has sounds, since one without a SoundTree has no way to reach them. Sounds are padded to
whole VAG blocks automatically; your original WAV is never modified.

## Search

The search box matches resource names and text content across *every* dumped RCO. Dump the
whole firmware resource folder once and "which RCO has X?" becomes instant.

## Migrate < 4.89

Firmware 4.89 shifted the "override" pointers that objects use to read the XMB layout tables,
which broke the positioning of every mod made before it. **Migrate < 4.89** re-bases the
checked mods onto current firmware: for each object it copies the six override fields from the
matching object in bundled 4.93 data, matched by RCO and object name. Everything else — your
images, positions, colours — is left exactly as you made it, only the overrides move.

It's bundled, so there's no folder to point at; objects Sony removed between firmwares are
reported in the log, the change is revertable, and re-running it does nothing (idempotent).
Then Compile as normal. It targets current firmware (4.93); a mod already on current firmware
is untouched.

## Deploy to the PS3

**Deploy** uploads the checked *compiled* RCOs straight to the console's `dev_blind` over FTP,
so you can try a theme without swapping files by hand. Set your console's IP as `ps3ip` in
`settings.txt`, and run an FTP server on it (e.g. simple-ftp). Deploy lights up once a checked
row is compiled; it checks the console is reachable, warns before writing (a bad RCO can stop
the XMB loading — there is no undo), uploads to `/dev_blind/vsh/resource/`, and tells you to
restart the XMB. No backups are kept: if the XMB won't load, the FTP server won't either, so a
backup couldn't be pushed back anyway.

## Sets and patches

- **Save Set… / Load Set…** — a `.rcoset` is just the list of source RCO paths a theme spans,
  so you can reload the whole set in one go. Loading a set re-uses dumps you already have
  rather than re-dumping them.
- **Export Patch…** — bundles only the files you changed into a shareable `.rcopatch` (a zip
  with `<rcoName>/<file>` entries). It contains your edits, not Sony's files, so it is safe to
  share and reproduces your mod on anyone's own dumps.
- **Apply Patch…** — copies a patch's files onto your dumps, then you Compile. It names any
  RCOs the patch needs that you haven't dumped yet, and warns before overwriting any file you
  have edited yourself (applying has no undo).

## Settings

`settings.txt` sits next to the exe, `key=value` with `#` comments, created on first run.
A tool that isn't installed simply greys out its menu entry, so the defaults are safe:

- `imageEditor` — editor for right-click **Edit** on an image (default: Paint.NET).
- `textEditor` — editor for right-click **Edit** on an XML/TXT file (default: Notepad++).
- `diffTool` — compare tool for right-click **Diff** (default: WinMerge).
- `ps3ip` — the console's IP address, used by **Deploy** (empty by default).

## GIM format fidelity

Edited PNGs are re-encoded by reading the original GIM's header: byte order (PS3 big-endian),
pixel format (RGBA8888/5551/4444/5650, indexed, DXT1/3/5) and pixel storage order (normal vs
PSP "faster"). GimConv has no stock flags for PS3 byte order or DXT, so the bundled
`GimConv.cfg` adds `-rcops3` and `-bppdxt1/3/5`. Verified on hardware: a rebuilt RCO works on
a real PS3, and a full dump→edit→compile→re-dump round trip compares byte-identical.

## Notes

- A re-dump always produces a fresh dump. Re-adding the same RCO from the same file keeps the
  existing dump and its edits; re-dumping from a *different* source file (e.g. another firmware)
  warns first, because edits belong to the file they were made against and can't carry over.
- Some OSK images (index8, paletted) can't be decoded by GimConv and show as raw tiles; they
  still round-trip untouched.
- All the firmware's text languages are named, including the four the stock rcomage table
  didn't cover (Polish, Portuguese-BR, English-UK, Turkish), added to the bundled `miscmap.ini`.
- Each dump keeps its pristine copy in a hidden `.original` folder inside the dump; leave it be.

## Build

.NET Framework 4.0 WPF, no external dependencies. Build `rco-studio.csproj` with
MSBuild / Visual Studio. The bundled `tools/` (rcomage + GimConv) are copied to the output
folder automatically.
