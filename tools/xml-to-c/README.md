# xml-to-c

Tiny .NET console app that turns a screen XML layout into a `.c` / `.h`
pair compatible with `simple-lib-app`.

Drop an XML onto `xml-to-c.exe` (or pass its path as argv). On success the
generator writes `<name>.c` and `<name>.h` next to the input and exits silently.
On any validation or parse error it prints a message with a line number and
pauses so you can read it.

```
xml-to-c <screen.xml> [cPath] [hPath]
```

Both output paths are optional. Each may be a full file path or a directory
(in which case `<name>.c` / `<name>.h` is appended). When omitted they default
to the input's directory. Missing parent directories are created.

Targets `.NET Framework 3.5`. Open `xml-to-c.csproj` in Visual Studio 2010+
or build with MSBuild.

## XML schema

Root must be `<screen name="..." [fill="..."]>`. The `name` becomes the
`<name>Screen` C variable and the output filenames `<name>.c` / `<name>.h`.
`fill` is optional; when present it's emitted as `gfxClear(...)` at the top
of the draw fn.

Children fall into two groups: **resources** (declared once, opened/loaded in
init) and **drawables** (run in document order in the draw fn).

### Resources

```xml
<Font    id="pop"   type="FONT_POP"/>
<Texture id="icons" path="/dev_hdd0/game/APP00001/USRDIR/icons.png"/>
```

* `<Font>` — wraps `fontOpenSystem(type)`. Reference from drawables as
  `font-id="pop"`. Valid types: `FONT_POP`, `FONT_GOTHIC_JP`, `FONT_SANS`,
  `FONT_SERIF`. Closed in term.
* `<Texture>` — wraps `gfxLoadTexture(path)`. Reference from `<Image>` as
  `texture-id="icons"`.

### Drawables

Position and size use comma-separated pair attributes:

| Attribute | Format   | Notes                                |
|-----------|----------|--------------------------------------|
| `xy`      | `"X,Y"`  | required on all positioned elements  |
| `wh`      | `"W,H"`  | required or optional depending on element; accepts `"auto"` |

```xml
<Rectangle  id="..." xy="0,0" wh="100,50" fill="COLOR_RED_500"
         corner-radius="6" border-thickness="2" border-color="0x333333"/>

<Circle     id="..." xy="100,100" radius="40" fill="COLOR_BLUE_500"/>

<Triangle   id="..." p0="0,0" p1="50,100" p2="100,0" fill="COLOR_GREEN_500"/>

<Line       id="..." from="0,0" to="200,0" thickness="2" color="COLOR_WHITE"/>

<Image      id="..." texture-id="icons" xy="42,20"
         wh="auto" src-xy="10,132" src-wh="68,57"
         filter="nearest"/>

<Label      id="..." xy="10,10" wh="auto"
         content="Hello" font-id="pop" size="24"
         color="COLOR_WHITE" wrap="nowrap"/>

<Breadcrumb id="..." xy="40,100" wh="1840,43"
         font-id="pop" font-size="18"
         bg-color="0x010B1C" border-color="0x161C2C"
         text-color="COLOR_WHITE" chevron-color="0x404653"
         corner-radius="5" border-thickness="2">
   <segment text="root" />
   <segment text="child" />
</Breadcrumb>
```

* `<Rectangle>` — emits `rectangleInit` / `rectangleDraw`. Optional
  `corner-radius`, `border-thickness`, `border-color`.
* `<Circle>` — emits `circleInit` / `circleDraw`. Optional `border-thickness`,
  `border-color`.
* `<Triangle>` — emits `triangleInit` / `triangleDraw`. Points are `p0`, `p1`,
  `p2` as comma-separated float pairs. Optional `border-thickness`,
  `border-color`.
* `<Line>` — emits `lineInit` / `lineDraw`. `from`/`to` are required
  `"X,Y"` integer pairs.
* `<Image>` — emits `imageInit` / `imageDraw`. `wh` is optional (defaults to
  auto-size from texture). `src-xy` and `src-wh` define an optional source
  rectangle in pixel coordinates for spritesheet extraction; when omitted the
  full texture is used. `filter` accepts `nearest` (default) or `linear`.
* `<Label>` — emits `labelInit` / `labelDraw`. `wh` is optional (defaults to
  auto-size). `wrap` accepts `wrap`, `nowrap` (default), or `ellipsis`.
* `<Breadcrumb>` — emits `breadcrumbInit` / `breadcrumbDraw`. Contains
  `<segment text="..."/>` children.

### Shared rules

* Element `id` values must each be a valid C identifier and unique within the
  screen (resources and drawables share the namespace because they all become
  file-scope statics).
* `name` is an optional human-readable label; when present it appears as a
  trailing `// name` comment on the draw call.
* Color attributes accept either a named constant from `colors.h`
  (`COLOR_RED_500`, `COLOR_WHITE`, …) or a literal `0xRRGGBB` / `0xAARRGGBB`
  hex value. Six-digit hex is expanded to `0xFFRRGGBB`.

### Comments

XML comments are passed through to the generated `.c`. Each `<!-- ... -->`
block attaches to the element that immediately follows it and is emitted as
one or more `// ...` lines just before that element's call.

The generator does not emit any comments of its own — what you see in the
output is what you wrote in the XML.

## Generated layout

Emitted `.c` follows the `Screen` pattern from `simple-lib-app`:

* opens fonts and loads textures in `<name>Init`, then initialises each
  drawable component
* draws in document order in `<name>Draw`
* closes fonts and frees breadcrumbs in `<name>Term`
* `Resume`, `Update`, `Suspend` are emitted as empty stubs — write your own
  if/when you need interaction; this generator targets static visual layouts.

## Examples

* `sample.xml` — small mixed-element demo.
* `palette.xml` — replicates `apps/app-sample/src/screens/palette.c`
  (26 hues × 11 shades = 286 cells plus a "Colors" title).
