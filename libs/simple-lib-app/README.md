# simple-lib-app

Reusable static library for PS3 homebrew **apps** (NPDRM EBOOTs). Sony official SDK 4.75.

It provides the whole drawing-and-input side of an app: an RSX 2D renderer, fonts and text, a
flat/Metro UI widget set, controller input, and a screen/overlay framework. Apps link
it and build their screens on top; the file manager, yo-player, and the Ren'Py/video players all use it.

Audio and video *playback* live in the separate `simple-lib-av` library — this lib only draws (it does
own the zero-copy path that puts a decoded video frame on screen).

## What it provides

### Graphics (`gfx.h`)

2D renderer over the RSX (the PS3's GPU). Colours are `0xAARRGGBB`, origin top-left.

- **Frame loop**: `initGfx` / `beginGfxFrame` / `clearGfx` / `endGfxFrame` / `termGfx`, with vsync
  control (`setGfxVsync`) and a flip-wait timing readout (`getGfxFlipWaitUs`).
- **Flat shapes** (the Metro look — no sprites needed): `fillGfxRectangle`, `strokeGfxRectangle`
  (border only, given a thickness), `drawGfxBox` (fill + border in one call), `fillGfxCircle`,
  `drawGfxTriangle`, `drawGfxLine`.
- **Textures**: `loadGfxTexture` (PNG/JPEG file), `loadGfxTextureMem` (in-memory image),
  `uploadGfxTexture` / `updateGfxTexture` (raw ARGB), `drawGfxTexture`, `freeGfxTexture`.
- **VRAM allocator**: a coalescing free-list — `allocateVram` / `freeVram`, each owner frees what it
  allocated, plus usage readouts (`getUsedVram`, `getFreeVram`, `getLargestFreeBlock`).
- **Render-to-texture**: `createGfxRenderTarget` / `beginGfxRenderTarget` / `endGfxRenderTarget` /
  `freeGfxRenderTarget` for cross-fades, caching a scene, post effects.
- **Video path (zero-copy)**: `allocGfxVideoBuffer` + `drawGfxYuvFrame` decode YUV planes straight
  into RSX-visible memory and convert to RGB in a shader — no per-frame copies.
- **Front-buffer read** (`getGfxDisplayBuffer`) and letterbox helper (`getGfxLetterboxRect`).

### Fonts and text (`font.h`, `ui/label.h`)

TrueType/OpenType rendering via libfont/FreeType, with word wrap, ellipsis, measurement, alignment,
drop shadows, and a typewriter-reveal layout report.

- Text is **rasterised into a VRAM texture once** (`TextTexture`, `renderFont*`) and then drawn as a
  quad each frame. This is the important constraint: do **not** re-rasterise text every frame — it
  wedges the RSX. Render on change, draw the cached texture the rest of the time.
- `Label` wraps a `TextTexture` with position/size/colour. `initLabel` / `setLabelText` re-rasterise;
  `drawLabel` / `drawLabelAlpha` just draw. `initLabelRaw` renders text literally (no `{i}`/`{color=}`
  markup parsing) — use it for untrusted content like filenames.
- `setLabelColor` re-rasterises the current text in a new colour — this is the hook that makes live
  theme switching work (the colour is baked into the texture, so changing it means re-rendering).

### Widgets (`ui/`)

Flat/Metro components. The library stays **theme-agnostic**: the app passes colours in, and most
widgets expose a `retheme*` call to recolour on a live theme switch.

- **Text/records**: `label`, `breadcrumb` (navigable path with code-drawn `>` separators),
  `button` (icon + label, enabled/disabled), `button-hints` (a centred "[glyph] caption" footer row).
- **Chrome**: `scrollbar` (proportional thumb), `checkbox` (empty/ticked box glyphs from the icon font),
  `slice` / nine-slice (`slice.h`, stretch a small sprite to any size), `stats` (FPS/VRAM overlay,
  L3+R3 toggle), `volume-meter` (left-edge pill column with auto-hide; its speaker is an icon glyph).
- **Icon font** (`ui/icon-font.h`, `ui/icon-ids.h`): scalable, theme-tintable UI icons from a single
  embedded TTF (built in Fontello). `initIconFont()` loads it once; `initIcon(&icon, ICON_FOO, size)`
  binds a glyph by name and `drawIcon(&icon, x, y, colour)` draws it tinted at its natural size (the
  glyph rasterises once into a shared cache and stays crisp at any size). Any app that links the lib
  gets the icons for free — no shipped image assets. The TTF, `icon-data.c` and `icon-ids.h` are
  generated from `icons/config.json` by sprite-packer's icons mode (a gated pre-build step).
- **Primitive shapes** as tiny value types: `rectangle`, `circle`, `line`, `triangle`, `image`
  (positioned texture with alpha).
- **On-screen input**: `keyboard` and `hex-pad` are controller-driven text/hex entry docked
  bottom-right, both built on the shared `key-grid` engine (`KeyGridTheme` supplies the flat palette;
  holding L2 hands the d-pad to the document underneath so a caret can move without closing the grid).
- **Console button art** (`console-glyphs`): decodes the PS3's own controller glyphs (Cross, L1, …)
  from `/dev_flash` at runtime, so apps render native XMB-style hints without shipping art. Pairs with
  `button-hints`. Needs vsh-class filesystem privilege (the repo's NPDRM apps have it).

### Input (`pad.h`, `button-repeat.h`)

- `pad`: poll the controller once per frame (`updatePad`), then query per-button state —
  `isPadButtonPressed` (the frame it went down), `isPadButtonHeld`, `isPadButtonDown`,
  `isPadButtonReleased`, plus analog sticks.
- `button-repeat`: turns a held button into an accelerating repeat signal (`isRepeatDue`) for scrolling.

### App framework

- **Screens** (`screen.h`, `screen-manager.h`): a `Screen` is an init/resume/update/draw/suspend/term
  vtable; the screen-manager owns the active screen and a navigation stack (`changeScreen`,
  `pushScreen`, `popScreen`).
- **Overlays** (`overlay.h`): a lifecycle-managed layer drawn on top of a screen (visible/hidden),
  driven through inline `showOverlay` / `hideOverlay` / `updateOverlay` / `drawOverlay` helpers.
- **App loop** (`app.h`): XMB-exit handling (`appRegisterExitCallback`, `appPoll`, `requestAppExit`,
  the shared `appExitRequested` flag) and cold-start network/RTC bring-up (`initNet`, `initRtc`) —
  apps run as cold NPDRM processes, so the network stack isn't live until `initNet`.

### Other helpers

- **Images** (`image-loader.h`): asynchronous PNG/JPEG decode on a background worker thread (decodes to
  a heap ARGB buffer; the main thread uploads to VRAM), so large images don't freeze the UI.
  Generation-based request superseding, oversized-JPEG downscaling, 4096×4096 cap.
- **On-screen keyboard** (`osk-input.h`): non-blocking wrapper around the system OSK (`cellOskDialog`),
  driven by the app's existing `appPoll`; result comes back through a completion callback.
- **File tasks** (`file-task.h`): run one long copy/move/delete on a background thread with byte
  progress and a cancel flag while the UI keeps rendering.
- **Animation** (`anim.h`): easing curves and colour interpolation. **Timer** (`timer.h`): polling
  interval timer. **Colours** (`colors.h`): the Tailwind v4 palette as named constants.
- **Google Drive** (`gdrive.h`, `gdrive-crypto.h`): mounts a Google Drive account as a VFS volume, so an
  app browses, downloads from and uploads to it with the ordinary `openFs`/`readFs`/`writeFs` calls.
  Uploads stream in 1 MB pieces through Drive's resumable-upload sessions, so file size isn't bound by
  memory. The OAuth credentials come from the app's settings.txt and are re-saved encrypted against the
  console's own id on first successful sign-in (freshly pasted plaintext keys always win over the stored
  blob, which is how an expired token is recovered from). `initGdrive` mounts nothing when the settings
  hold no keys, so an app that never configures it pays only the link cost. Lives here rather than in
  core because it needs malloc and HTTP; needs `initModernHttp()` at startup.
- File-system helpers come from `simple-lib-core` (`file.h`) — apps include `"file.h"` and get the core
  copy via the include path.

## Usage

1. Build `simple-lib-app` first — produces `libsimple-lib-app.a` in `bin/Release/`.
2. In your app's vcxproj:
   - Add `$(SolutionDir)libs\simple-lib-app\include` to include directories.
   - Link with `-L"$(SolutionDir)libs\simple-lib-app\bin\$(Configuration)" -lsimple-lib-app`.
   - Add `-lpngenc_stub` only if you save PNGs (`savePngArgb`).
3. Include headers as needed: `#include "gfx.h"`, `#include "font.h"`, `#include "ui/label.h"`, etc.

Shaders (`vpshader.cg`, `fpshader.cg`, `fpshader-yuv.cg`) are compiled as custom build steps inside
this library — consuming apps do not need their own shader build steps.

## Key constraints

- **Never rasterise text every frame.** Text and labels bake into a VRAM texture; re-rendering it per
  frame wedges the RSX. Re-render only when the text or colour actually changes (`setLabelText`,
  `setLabelColor`), and draw the cached texture otherwise.
- **Theme colours are baked in.** Widgets don't hold "live" colours — a theme switch re-rasterises the
  affected text/widgets via the `retheme*` / `setLabelColor` calls. The library itself is
  theme-agnostic; the app owns the palette.
- **This lib is app-only — do not use it from a VSH PRX.** Unlike `simple-lib-core` (which is
  malloc-free and safe to link into VSH plugins), this library uses libc, libfont, and the RSX. It
  depends on `simple-lib-core` for cross-context primitives (printf, dbg, file, thread, string
  utilities, wire, log-backlog, bridge-client) and does **not** depend on `simple-lib-plugin`
  (PRX-only extras).
