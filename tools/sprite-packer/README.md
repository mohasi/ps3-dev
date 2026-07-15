# sprite-packer

Packs a folder of `.png` sprites into a single spritesheet `.png` and writes a
C header that names each sprite and gives its position in the sheet. Drawing
from one big texture with named regions is faster and simpler on the PS3 than
loading dozens of separate images.

It has a second job too: an **`icons` mode** that turns a Fontello icon font
(`icons.ttf` + `config.json`) into the embedded C font data and a name→codepoint
header, so the UI icon set regenerates on build the same way the sprite sheet
does. See "Icon font mode" below.

## How it's invoked

Apps that still use a sprite sheet run it automatically as a pre-build step.
`renpy-player.vcxproj` is the remaining one (its hand cursor):

```
"$(SolutionDir)tools\sprite-packer.exe" "$(ProjectDir)sprites" -o "$(ProjectDir)res" -h "$(ProjectDir)include"
```

So the sprite folder is the source of truth: add a PNG and it gets an enum entry
on the next build; delete a PNG and its entry disappears. You can also run it by
hand, or drag a folder onto the exe.

## Usage

```
sprite-packer <inputDir> [-o outputDir] [-n name] [-h headerDir] [-p prefix]
```

| Flag | Description | Default |
|------|-------------|---------|
| `-o` | Output directory for the `.png` | input directory |
| `-n` | Base name for the `.png` file (without extension) | input directory name |
| `-h` | Output directory for the `.h` file | same as `-o` |
| `-p` | Prefix for the enum, table, and header filename | `sprite` |

By default the header is `sprite-regions.h` and the table is `spriteRegions[n]`.
With `-p foo` they become `foo-regions.h` and `fooRegions[n]` (enum `FooId`,
members `FOO_*`).

## What it does

1. Loads every `.png` in the input directory (sorted alphabetically), each
   converted to 32-bit ARGB.
2. Packs them into the smallest power-of-2 sheet using a shelf algorithm with a
   1px transparent border between sprites.
3. Writes the spritesheet `.png` and the C header.

## Generated header

The header (`#pragma once`, includes `gfx.h`) contains:

- an `enum SpriteId` whose entries come from the filenames (uppercased,
  non-alphanumeric characters become `_`, behind the `SPRITE_` prefix), in
  packed order;
- a `static const SpriteRegion spriteRegions[n]` table indexed by that enum,
  each row `{ x, y, width, height }`;
- a `#define SPRITE_FULL ((SpriteRegion){0})` for whole-texture references.

## Example

```
sprite-packer sprites/ -o res/ -h include/
```

Produces `res/sprites.png` and `include/sprite-regions.h`.

## Icon font mode

`simple-lib-app` ships a single UI icon font (built in [Fontello](https://fontello.com/)):
`icons/icons.ttf` plus its `icons/config.json`. This mode reads those two files and
regenerates the C the app links against — the font bytes and the icon names:

```
sprite-packer icons <config.json> <icons.ttf> -c <dataOut.c> -i <idsOut.h>
```

`simple-lib-app.vcxproj` runs it as a pre-build step:

```
"$(SolutionDir)tools\sprite-packer.exe" icons "$(ProjectDir)icons\config.json" "$(ProjectDir)icons\icons.ttf" -c "$(ProjectDir)src\ui\icon-data.c" -i "$(ProjectDir)include\ui\icon-ids.h"
```

So `icons/` is the source of truth: export a fresh set from Fontello over those
two files, build, and both generated files update. Because Fontello keeps each
icon's codepoint in `config.json` (re-import it before editing), the names and
codepoints stay stable as you add or remove icons.

Generated:

- `icon-data.c` — `const unsigned char iconFontData[]` (the whole `.ttf`) plus
  `iconFontDataSize`, so the font is embedded and any app linking the lib gets the
  icons with no shipped asset.
- `icon-ids.h` — a `typedef enum IconId` with one `ICON_<UPPER_SNAKE>` per glyph
  (same naming rule as the sprite enum), each set to the font's codepoint.

Note: this reuses the same exe, so if you change the generator, rebuild
`sprite-packer` before the library build that calls it.
