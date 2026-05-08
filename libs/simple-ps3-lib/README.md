# simple-ps3-lib

Reusable static library for PS3 homebrew apps. Sony official SDK 4.75.

## Features

- **gfx** - RSX 2D renderer: batched textured quads, PNG loading, sprites, circles, rectangles, VRAM lifetime management (persistent/per-frame)
- **font** - system font rendering via libfont/FreeType: word wrap, ellipsis, text measurement, bake-to-texture
- **audio** - WAV playback (memory-loaded), OGG Vorbis streaming, master volume control
- **pad-input** - controller polling with press/release/held state tracking
- **screen** - stack-based screen lifecycle (init/resume/update/draw/suspend/term)
- **overlay** - persistent UI layer type (init/show/hide/update/draw/term)
- **app** - top-level application loop tying screens, overlays, input, and rendering together
- **anim** - easing curves, color interpolation, and animation containers
- **colors** - Tailwind color palette
- **file** - cellFs helpers (readFile, readFileAlloc, writeFile, fileExists, makeDir)
- **ui** - reusable UI components (label, breadcrumb, image, circle, line, rectangle, triangle)
- **shaders** - RSX vertex/fragment shader compilation (vpshader.cg, fpshader.cg) built as part of the library

## Usage

1. Build `simple-ps3-lib` first - produces `libsimple-ps3-lib.a` in `bin/Release/`.
2. In your app's vcxproj:
   - Add `$(SolutionDir)libs\simple-ps3-lib\include` to include directories.
   - Link with `-L"$(SolutionDir)libs\simple-ps3-lib\bin\$(Configuration)" -lsimple-ps3-lib` in additional linker options.
3. Include headers as needed: `#include "gfx.h"`, `#include "font.h"`, `#include "ui/label.h"`, etc.

## Layout

```
simple-ps3-lib/
+-- simple-ps3-lib.vcxproj
+-- vpshader.cg             # RSX vertex shader source
+-- fpshader.cg             # RSX fragment shader source
+-- include/                # public headers
|   +-- gfx.h
|   +-- font.h
|   +-- audio.h
|   +-- pad-input.h
|   +-- screen.h
|   +-- overlay.h
|   +-- app.h
|   +-- anim.h
|   +-- colors.h
|   +-- file.h
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
|   +-- font.c
|   +-- audio.c
|   +-- pad-input.c
|   +-- screen.c
|   +-- anim.c
|   +-- vorbis.c           # stb_vorbis, included by audio.c
|   +-- ui/
|       +-- breadcrumb.c
|       +-- label.c
```

## Notes

- Shaders (`vpshader.cg`, `fpshader.cg`) are compiled as custom build steps within this library. Consuming apps no longer need their own shader build steps.
- `vorbis.c` is stb_vorbis (public domain), `#include`d directly by `audio.c` - not compiled separately.
- UI components under `ui/` are self-contained drawing helpers. Header-only components (circle, image, line, rectangle, triangle) define inline draw functions; label and breadcrumb have separate implementation files.
- This lib is independent of `simple-ps3-prx-lib`. Both contain their own copy of `file.h` since it is context-neutral.
