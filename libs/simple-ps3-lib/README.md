# simple-ps3-lib

Reusable static library for PS3 homebrew apps. Sony official SDK 4.75.

## Features

- **gfx** — RSX 2D renderer: batched textured quads, PNG loading, sprites, circles, rectangles, VRAM lifetime management (persistent/per-frame)
- **font** — system font rendering via libfont/FreeType: word wrap, ellipsis, text measurement, bake-to-texture
- **audio** — WAV playback (memory-loaded), OGG Vorbis streaming, master volume control
- **pad-input** — controller polling with press/release/held state tracking
- **screen** — stack-based screen lifecycle (init/resume/update/draw/suspend/term)
- **overlay** — persistent UI layer type (init/show/hide/update/draw/term)
- **anim** — easing curves, color interpolation, and animation containers
- **colors** — Tailwind color palette
- **file** — cellFs helpers (readFile, readFileAlloc, writeFile, fileExists, makeDir)

## Usage

1. Build `simple-ps3-lib` first — produces `libsimple-ps3-lib.a` in `bin/Release/`.
2. In your app's vcxproj:
   - Add `$(SolutionDir)libs\simple-ps3-lib\include` to include directories.
   - Link with `-L"$(SolutionDir)libs\simple-ps3-lib\bin\$(Configuration)" -lsimple-ps3-lib` in additional linker options.
3. Include headers as needed: `#include "gfx.h"`, `#include "font.h"`, etc.

## Layout

```
simple-ps3-lib/
├── simple-ps3-lib.vcxproj
├── include/            # public headers
│   ├── gfx.h
│   ├── font.h
│   ├── audio.h
│   ├── pad-input.h
│   ├── screen.h
│   ├── overlay.h
│   ├── anim.h
│   ├── colors.h
│   └── file.h
└── src/                # implementation
    ├── gfx.c
    ├── font.c
    ├── audio.c
    ├── pad-input.c
    ├── screen.c
    ├── anim.c
    └── vorbis.c        # stb_vorbis, included by audio.c
```

## Notes

- The renderer (`gfx.c`) expects compiled RSX shaders linked by the consuming app. The app provides `vpshader.cg` and `fpshader.cg` as custom build steps — the lib references them via `extern` symbols resolved at link time.
- `vorbis.c` is stb_vorbis (public domain), `#include`d directly by `audio.c` — not compiled separately.
- This lib is independent of `simple-ps3-prx-lib`. Both contain their own copy of `file.h` since it is context-neutral.
