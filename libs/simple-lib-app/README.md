# simple-lib-app

Reusable static library for PS3 homebrew apps. Sony official SDK 4.75.

## Features

- **gfx** - RSX 2D renderer: batched textured quads, PNG loading, sprites, circles, rectangles, and a coalescing free-list VRAM allocator (`allocateVram`/`freeVram`) where each owner frees what it allocated
- **image-loader** - asynchronous PNG/JPEG decoder: background worker thread decodes to heap ARGB8888 buffer, main thread uploads to VRAM. Prevents UI freezes on large images. Codec downscaling for oversized JPEGs, size validation to prevent RSX hangs. Includes directory listing of supported images (sorted case-insensitive).
- **font** - system font rendering via libfont/FreeType: word wrap, ellipsis, text measurement, bake-to-texture
- **audio** - multi-stream mixer playing WAV, OGG Vorbis, MP3 and FLAC. Streamed playback (WAV read from disk via dr_wav callbacks; OGG/MP3/FLAC decoded on demand from their compressed bytes) or fully memory-loaded for short SFX. Per-stream and master volume, seeking, a rolling waveform envelope for visualisers, and track-title (ID3 / Vorbis comment) extraction
- **pad-input** - controller polling with press/release/held state tracking
- **screen** - stack-based screen lifecycle (init/resume/update/draw/suspend/term)
- **overlay** - persistent UI layer type (init/show/hide/update/draw/term)
- **app** - top-level application loop tying screens, overlays, input, and rendering together
- **anim** - easing curves, color interpolation, and animation containers
- **colors** - Tailwind color palette
- **file** - cellFs helpers come from `simple-lib-core/file.h` (path utilities, mountDevBlind, etc.); apps include `"file.h"` and get the core copy via the include path. App-only heap helpers live alongside their single caller.
- **ui** - reusable UI components (label, breadcrumb, image, slice, circle, line, rectangle, triangle)
- **timer** - polling interval timer using RTC
- **stats** - toggleable on-screen diagnostics: FPS and VRAM used/free/largest-block, shown/hidden with L3+R3
- **shaders** - RSX vertex/fragment shader compilation (vpshader.cg, fpshader.cg) built as part of the library

## Usage

1. Build `simple-lib-app` first - produces `libsimple-lib-app.a` in `bin/Release/`.
2. In your app's vcxproj:
   - Add `$(SolutionDir)libs\simple-lib-app\include` to include directories.
   - Link with `-L"$(SolutionDir)libs\simple-lib-app\bin\$(Configuration)" -lsimple-lib-app` in additional linker options.
3. Include headers as needed: `#include "gfx.h"`, `#include "font.h"`, `#include "ui/label.h"`, etc.

## Layout

```
simple-lib-app/
+-- simple-lib-app.vcxproj
+-- vpshader.cg             # RSX vertex shader source
+-- fpshader.cg             # RSX fragment shader source
+-- include/                # public headers
|   +-- gfx.h
|   +-- image-loader.h
|   +-- font.h
|   +-- audio.h
|   +-- vorbis.h            # vendored single-header decoders (stb_vorbis / dr_libs),
|   +-- mp3.h               #   #included by audio.c, not compiled on their own
|   +-- flac.h
|   +-- wav.h
|   +-- pad-input.h
|   +-- screen.h
|   +-- overlay.h
|   +-- app.h
|   +-- anim.h
|   +-- colors.h
|   +-- ui/
|       +-- breadcrumb.h
|       +-- circle.h
|       +-- image.h
|       +-- label.h
|       +-- line.h
|       +-- rectangle.h
|       +-- triangle.h
+-- src/                    # implementation
|   +-- gfx.c
|   +-- image-loader.c
|   +-- font.c
|   +-- audio.c
|   +-- pad-input.c
|   +-- screen.c
|   +-- anim.c
|   +-- ui/
|       +-- breadcrumb.c
|       +-- label.c
```

## Notes

- Shaders (`vpshader.cg`, `fpshader.cg`) are compiled as custom build steps within this library. Consuming apps no longer need their own shader build steps.
- The audio decoders are vendored single-header libraries `#include`d directly by `audio.c` (not compiled separately): `vorbis.h` (stb_vorbis) and `mp3.h`/`flac.h`/`wav.h` (the dr_libs). `audio.c` defines each one's `*_IMPLEMENTATION` before including it.
- UI components under `ui/` are self-contained drawing helpers. Header-only components (circle, image, line, rectangle, triangle) define inline draw functions; label and breadcrumb have separate implementation files.
- `image-loader` uses a persistent background worker thread (spawned lazily on first request) to decode PNG/JPEG images without blocking the UI. The worker produces heap ARGB8888 buffers; the caller uploads to VRAM on the main thread. Generation-based request superseding lets the caller retarget in-flight decodes. Max texture dimension is capped at 4096×4096 (RSX limit); JPEGs are codec-downscaled (1/2/4/8) to fit, PNGs are rejected if oversized.
- This lib depends on `simple-lib-core` for the cross-context
  primitives (printf, dbg, file, thread, string utilities, wire,
  log-backlog, bridge-client). It does **not** depend on
  `simple-lib-plugin` (PRX-only extras).
