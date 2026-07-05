# simple-lib-app

Reusable static library for PS3 homebrew apps. Sony official SDK 4.75.

## Features

- **gfx** - RSX 2D renderer: batched textured quads, PNG loading, sprites, circles, rectangles, a
  coalescing free-list VRAM allocator (`allocateVram`/`freeVram`) where each owner frees what it
  allocated, and the zero-copy video draw path (`allocGfxVideoBuffer` + `drawGfxYuvFrame`: YUV
  planes in RSX-mapped main memory sampled by a BT.709 fragment shader)
- **image-loader** - asynchronous PNG/JPEG decoder: background worker thread decodes to heap
  ARGB8888 buffer, main thread uploads to VRAM. Prevents UI freezes on large images. Codec
  downscaling for oversized JPEGs, size validation to prevent RSX hangs. Includes directory listing
  of supported images (sorted case-insensitive)
- **image-encoder** - ARGB-to-PNG encode for screenshots/thumbnails
- **font** - system font rendering via libfont/FreeType: word wrap, ellipsis, text measurement, bake-to-texture
- **pad / button-repeat** - controller polling with press/release/held state tracking and hold-to-repeat helpers
- **screen / screen-manager / overlay / app** - screen lifecycle stack, persistent UI layers, and the top-level application loop tying them to input and rendering
- **osk-input** - on-screen keyboard wrapper
- **file-task** - background file operations (copy/move/delete) with progress callbacks
- **anim** - easing curves, color interpolation, and animation containers
- **colors** - Tailwind color palette
- **file** - cellFs helpers come from `simple-lib-core/file.h` (path utilities, mountDevBlind, etc.); apps include `"file.h"` and get the core copy via the include path
- **ui** - reusable UI components (label, breadcrumb, image, slice, button, checkbox, scrollbar, progress-bar, dialog-panel, circle, line, rectangle, triangle, stats)
- **timer** - polling interval timer using RTC
- **shaders** - RSX vertex/fragment shaders (vpshader.cg, fpshader.cg, fpshader-yuv.cg) compiled as part of the library

Audio and video playback live in `simple-lib-av` (the mixer moved there); this lib provides the
drawing side only.

## Usage

1. Build `simple-lib-app` first - produces `libsimple-lib-app.a` in `bin/Release/`.
2. In your app's vcxproj:
   - Add `$(SolutionDir)libs\simple-lib-app\include` to include directories.
   - Link with `-L"$(SolutionDir)libs\simple-lib-app\bin\$(Configuration)" -lsimple-lib-app` in additional linker options.
3. Include headers as needed: `#include "gfx.h"`, `#include "font.h"`, `#include "ui/label.h"`, etc.

## Notes

- Shaders (`vpshader.cg`, `fpshader.cg`, `fpshader-yuv.cg`) are compiled as custom build steps
  within this library. Consuming apps no longer need their own shader build steps.
- UI components under `ui/` are self-contained drawing helpers. Header-only components define
  inline draw functions; label and breadcrumb have separate implementation files.
- `image-loader` uses a persistent background worker thread (spawned lazily on first request) to
  decode PNG/JPEG images without blocking the UI. The worker produces heap ARGB8888 buffers; the
  caller uploads to VRAM on the main thread. Generation-based request superseding lets the caller
  retarget in-flight decodes. Max texture dimension is capped at 4096×4096 (RSX limit); JPEGs are
  codec-downscaled (1/2/4/8) to fit, PNGs are rejected if oversized.
- This lib depends on `simple-lib-core` for the cross-context primitives (printf, dbg, file,
  thread, string utilities, wire, log-backlog, bridge-client). It does **not** depend on
  `simple-lib-plugin` (PRX-only extras).
