# app-sample

PS3 homebrew demo showcasing engine features. Sony official SDK 4.75, targets EVILNAT 4.93 CFW.

## Features

- **RSX 2D renderer** - batched textured quads, PNG loading, sprite sheet animation, circles, rectangles
- **VRAM management** - coalescing free-list allocator with explicit per-allocation free; each screen/overlay/widget frees its own VRAM in `term()`
- **System font rendering** - libfont/FreeType with word wrap, ellipsis truncation, text measurement, and bake-to-texture
- **Audio** - WAV playback (memory-loaded), OGG Vorbis streaming, master volume control
- **Animation** - easing curves (quad/cubic/back/bounce/elastic), ping-pong/once modes, completion callbacks
- **Input** - controller polling with press/release/held state tracking
- **Screen lifecycle** - stack-based navigation with init/resume/update/draw/suspend/term
- **Overlays** - persistent UI layers (stats, side panel) with show/hide and self-managed status
- **Vsync control** - configurable via enum at init (GFX_VSYNC_OFF for uncapped, GFX_VSYNC_ON for locked)
- **Tailwind color palette** - slate, amber, sky, emerald, red, white

## Layout

```
app-sample/
+-- app-sample.vcxproj
+-- app-sample.conf         # make_package_npdrm config
+-- PARAM.SFO               # pre-built binary
+-- ICON0.PNG               # 320x176
+-- include/                # public headers
|   +-- screens/            #   screen declarations
|   +-- overlays/           #   overlay declarations
+-- src/                    # implementation
|   +-- screens/            #   screen implementations
|   +-- overlays/           #   overlay implementations
|   +-- demos/              #   demo helper modules
+-- res/                    # assets (copied to USRDIR at build)
+-- bin/<Cfg>/              # OutDir + package staging
+-- obj/<Cfg>/              # IntDir
```

## Build pipeline

1. Sony GCC PS3 toolset compiles `src/*.c` and links to `bin/<Cfg>/app-sample.ppu.elf`.
2. PostBuild:
   - `make_fself_npdrm` wraps ELF as `USRDIR/EBOOT.BIN`
   - Assets copied from `res/` into `USRDIR/`
   - `make_package_npdrm` emits `.pkg`

## Identity

- TITLE_ID: `APPSMP001`
- Content_ID: `UP0001-APPSMP001_00-APPSAMPLEHBREW01`

## Architecture

### Screens

`Screen` struct with function pointers: `init`, `resume`, `update`, `draw`, `suspend`, `term`.
Status: `SCREEN_TERMINATED`, `SCREEN_INITIALISED`, `SCREEN_ACTIVE`, `SCREEN_SUSPENDED`.
Navigation via `changeScreen`, `pushScreen`, `popScreen`.

### Overlays

`Overlay` struct with function pointers: `init`, `show`, `hide`, `update`, `draw`, `term`.
Status: `OVERLAY_TERMINATED`, `OVERLAY_INITIALISED`, `OVERLAY_VISIBLE`, `OVERLAY_HIDDEN`.
Each overlay guards its own `update`/`draw` based on status - callers invoke unconditionally.
Parent screens own overlays as local state and manage their lifecycle manually.


## Dependencies

- **simple-lib-app** - provides the renderer, shader compilation, font, audio, input, screen/overlay lifecycle, animation, and UI components. Shader sources (vpshader.cg, fpshader.cg) live in the library and are compiled there.
