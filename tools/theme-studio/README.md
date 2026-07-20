# theme-studio

A Windows editor for PS3 themes, including true dynamic (3D) themes — 3D models, motion and lights,
which Sony's own theme creator never supported. Design and rationale:
[docs/theme-studio-design.md](../../../docs/theme-studio-design.md).

A `.themeproj` can be passed on the command line, so projects open by double-click.

## How it works

`ThemeProject` is the saved `.themeproj` and the source of truth: compilation is one-way, so a `.p3t`
can never be turned back into a project.

A `.themeproj` is **one self-contained file** — a zip (written through `System.IO.Packaging`) holding
`project.xml`, the scene script and a copy of every model, texture, picture and sound the theme uses,
the same idea as a `.docx`. `ProjectPackage` unpacks it to a temp folder while open and packs it back
on save, writing a temp file and swapping it in so a failed save cannot destroy the original. Older
XML-only projects (which stored fragile absolute paths) still open and are imported on the next save.

`ThemeBuild` checks every referenced file exists **before** writing anything, stages the assets into a
folder named after the theme, runs p3tcompiler and leaves the `.p3t` there — one folder per theme, so
projects sharing a directory never overwrite each other. `Ps3Deploy` uploads over FTP; Deploy builds
first, since there is no telling whether the last build still matches the project.

Every child process goes through `ToolRun`, which does the three things these tools need: run from
their own folder (they load sibling dlls), close stdin (p3tcompiler waits on Enter), and drain both
output streams at once (reading one to the end deadlocks when the other fills its pipe).

### The 3D scene

`SceneProject` holds models, materials, actors, one camera and up to two lights; `SceneBuild` writes
the scene XML and runs `raf_compiler` as part of a theme build. `DaeFile` reads COLLADA models for
both their extent (the auto-fit) and their triangles (the preview). A model with no placement of its
own is sized to about a third of the screen and put in the bottom-right corner, clear of the XMB's
menu — written into the script as visible lines, not applied silently (`ScenePlacement`).

`DaePlacement` works out where a model's geometry actually sits, which is not optional detail: a `.dae`
stores its shape wherever it sat in the artist's scene and places it with a scene-graph transform. The
offset can arrive three ways, all present in one Sony sample — baked into the vertices (`clock.dae`), a
node transform (`bg.dae`), or a skin and skeleton (`nose.dae`). Reading raw vertices and ignoring the
last two looks right in the preview but puts half the scene off screen on the console. Size is
**measured** through every vertex rather than from one axis, since a model may be built lying down,
turned oddly, or scaled unevenly.

Not yet handled: **an actor's rotation turns about the model's origin, not the centre of its shape**, so
an off-centre model swings rather than spins. The real fix is to stage a re-centred copy of the
geometry; until then a script should avoid rotating off-centre models.

### Motion

Movement is written by hand in PSJS, Sony's JavaScript dialect. `PsjsSnippets` seeds a new scene with a
commented starter naming its objects; "Insert example" offers ten worked examples, and Ctrl+Space
suggests from the API plus the scene's objects. "Validate" runs Sony's own `raf_script.exe`
(`PsjsCheck`) — the only authoritative verdict.

### What a script can do

PSJS is ordinary JavaScript (`var`, `function`, `if`, `for`, `Math`, `Date`) with two additions: a
point in space is three numbers in angle brackets, `<x, y, z>` (a colour is four, `<r, g, b, a>`), and
`->` reads one part, like `thing.position->y`. `for...in`, `with` and `this` are not available.

Everything below works on the console; the **preview** runs all of it faithfully except the three
noted at the end.

A script never creates or destroys anything — it takes hold of what the scene already has. Every scene
has a camera and two lights, always by these names; models get the names you gave them.

```js
var thing = new Actor("myModel");       // one of your models
var view  = new Camera("camera");       // the one camera, always "camera"
var main  = new Light("mainlight");     // the point light
var fill  = new Light("filllight");     // the even ambient glow
```

Anything that moves is set the same way — a target value, then how long it takes:

```js
thing.setPosition(<0, 1, 0>, 2);                                      // move over 2 seconds
thing.setPosition(<0, 1, 0>, 2, INTERPOLATION_BEZIER, <0.4,0,0.6,1>); // ease in and out
thing.position = <0, 1, 0>;             // no time given = jump there at once
```

| Object | Properties (read and set outright) | Moving setters (value, [seconds], [easing], [shape]) |
|---|---|---|
| **Actor** | `position` `rotation` `direction` `up` `scale` `color` `uv_scale` `uv_offset` `enable` | `setPosition` `setRotation` `setDirection` `setUp` `setScale` `setColor` `setUVScale` `setUVOffset` |
| **Camera** | `position` `direction` `up` `yfov` `ymag` `aspect` (read only) | `setPosition` `setDirection` `setUp` |
| **Light** | `position` `direction` `color` `attenuation` | `setPosition` `setDirection` `setColor` `setAttenuation` |

`enable = false` hides an object — the way to make something appear part way through. `aspect` is
`16/9` or `4/3`, for supporting both screen shapes from one theme.

```js
System.timer[0] = new IntervalTimer(1.5, breathe);   // repeat every 1.5s
System.timer[1] = new OneShotTimer(3.0, reveal);     // once, 3s from now
var now = new Date();                                // now.hours/.minutes/.seconds/.year/.month/.day
System.interval                                      // seconds the last frame took
System.resolution                                    // <width, height> of the screen
```

**Not shown in the preview** (they still work on the console): `setAnim*` / `getAnim*`, which blend a
model's baked animation (the preview cannot play baked animation); per-object `Actor.timer[]` (the
scene-wide `System.timer[]` is fully supported); and `printPerf` / `printHeap`, which have nothing to
show.

### Preview

`XmbPreview` draws a rough XMB on a 1920x1080 canvas in the console's own fonts; when the background is
the project's 3D scene, `ScenePreview` renders it with WPF 3D and composites the icons on top. It is a
likeness, not a facsimile: placement, size, texture and motion are faithful, shading is not. Point
lights draw a small coloured dot at their position (toggle with "Show lights"); the camera cannot
appear in its own view, so it is reported in words.

The scene *plays*. No JavaScript engine can run PSJS, so the editor reads and runs it itself
(`PsjsSyntax`, `PsjsNodes`, `PsjsValues`, `PsjsMachine`) at 30fps, moving already-built shapes rather
than re-decoding textures each frame, and only while the Preview stage is showing. A per-frame budget
guards against a runaway script taking the editor down. `DdsFile` decodes the console's textures so
surfaces show properly — several Sony parts are plain discs whose entire detail lives in the texture.

## Known gaps

The 4:3 background (`sd`) is carried through the project but has no UI, so themes built here have no
background on a 4:3 display. Stereo menu sounds cannot be authored (one file per sound slot). A full
audit is in [audits/theme-studio-2026-07-20.md](../../../audits/theme-studio-2026-07-20.md).
