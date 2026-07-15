# app-sample

A starter PS3 homebrew app. It is a bootable, installable package that runs one demo screen
showing off what the shared `simple-lib-app` engine can do — graphics, text, audio, controller
input, animation, and the screen/overlay lifecycle. Copy this folder to begin a new app; strip out
the demo bits and keep the plumbing.

Built with Sony's official SDK 4.75, targets EVILNAT 4.93 CFW. Boots from the XMB Games column.

## What it demonstrates

The app opens on the **demo screen**, which draws all of these at once:

- **2D graphics** — coloured rectangle bars, a box that bounces off the screen edges, a GPU
  gradient triangle (colour blended across its corners), a loaded PNG image, and a 23-frame sprite
  sheet playing as an animation.
- **Text** — a title label, a paragraph that word-wraps inside a box, and a line that gets cut off
  with an ellipsis when it is too long. All drawn with the system font.
- **Animation** — a circle slides left and right (eased, ping-pong) while its colour fades between
  white and red.
- **Audio** — a sound effect loaded fully into memory (Cross to play) and background music streamed
  from disk (Start to toggle), plus master-volume up/down.
- **Controller readout** — a live text display of every button and both analog sticks.
- **Overlay** — a side panel that slides in from the right (Select to toggle).
- **Screen navigation** — Right opens a second screen showing the full colour palette; Left returns.

A small **stats overlay** (frame rate and video-memory use) sits in the top-left. Click both sticks
(L3 + R3) together to toggle it.

### Controls

| Button | Action |
|---|---|
| Cross | play the sound effect |
| Start | toggle background music |
| Up / Down | raise / lower master volume |
| Right | open the palette screen |
| Left | back out of the palette screen |
| Select | toggle the side panel |
| L3 + R3 | toggle the stats overlay |

### Palette screen

A grid of all 286 built-in colours (26 hues by 11 shades) from `colors.h`, so you can see every
named colour the engine ships with.

## Identity

- **TITLE_ID:** `APPSMP001`
- **Title on XMB:** App Sample
- **Content_ID:** `HB0001-APPSMP001_00-APPSAMPLEHB00001`
- **Category:** HG (hard-drive game), bootable

`PARAM.SFO.xml` is the human-editable source for the app's metadata (title, version, firmware
requirement, allowed resolutions and audio formats); the build turns it into the binary `PARAM.SFO`.
`app-sample.conf` sets the packaging options (Content_ID, free/homebrew DRM, bootable game type).

## Layout

```
app-sample/
├── app-sample.vcxproj
├── app-sample.conf            # packaging config for make_package_npdrm
├── PARAM.SFO.xml              # editable metadata source
├── PARAM.SFO                  # built binary metadata
├── ICON0.PNG                  # game icon (320x176)
├── include/                   # headers
│   ├── screens/               #   screen declarations
│   ├── overlays/              #   overlay declarations
│   └── *.h                    #   demo module declarations
├── src/                       # source
│   ├── main.c                 #   entry point and main loop
│   ├── screens/               #   demo and palette screens
│   ├── overlays/              #   side panel
│   └── demos/                 #   small self-contained demo modules
├── res/                       # assets copied into USRDIR at build time
├── bin/<Config>/              # build output + package staging
└── obj/<Config>/              # intermediate build files
```

## How it is structured

`main.c` sets up the engine (graphics, audio, font, controller), starts the demo screen, and runs
the main loop: poll input, update the active screen, then draw a frame. Cleanup happens in reverse
on exit.

**Screens** are the top-level views. Each is a `Screen` struct of function pointers —
`init`, `resume`, `update`, `draw`, `suspend`, `term` — and a status. You move between them with
`changeScreen`, `pushScreen`, and `popScreen` (a stack, so the palette pushes on top of the demo and
pops back). Each screen frees everything it allocated in `term`.

**Overlays** are UI layers that sit on top of a screen (the side panel here). Same idea — a struct of
function pointers plus a status — but the parent screen owns the overlay and drives its lifecycle.
An overlay checks its own status inside `update`/`draw`, so the caller can invoke them every frame
without guarding.

The **demo modules** under `src/demos/` (bars, bouncing box, gradient triangle, animated sprite,
pad display) are deliberately tiny and single-purpose — each is one clean example of a feature.

## Build and deploy

Everything goes through the **ps3 MCP tool** (which builds in a Windows build VM and installs to the
live PS3):

1. `list` — confirm the valid names.
2. `build` with kind `apps` and name `app-sample` — returns a job id.
3. `poll` that job id until it finishes; exit code 0 means success.
4. `deploy` with name `app-sample` to install it on the console.

The build compiles the C sources, links against `simple-lib-app`, wraps the result as
`EBOOT.BIN`, copies the `res/` assets into `USRDIR/`, and produces the installable `.pkg`.

## Using it as a starting point

1. Copy this folder to `dev/apps/your-app`.
2. Give it a new identity: change `TITLE_ID`, `TITLE`, and `Content_ID` in `PARAM.SFO.xml` and
   `app-sample.conf`, and rename the `.vcxproj`. The `TITLE_ID` appears in the asset paths in the
   source (e.g. `/dev_hdd0/game/APPSMP001/USRDIR/...`) — update those too.
3. Replace the demo screen with your own: keep the `Screen`/`Overlay` pattern, drop the demo modules
   you do not need, and swap the assets in `res/`.
4. Build and deploy as above.

## Dependencies

- **simple-lib-app** — the shared engine: 2D renderer, system font, audio, controller input,
  animation, the screen/overlay lifecycle, and the reusable UI pieces (labels, rectangles, circles,
  stats). The graphics shaders live in that library and are compiled there.
