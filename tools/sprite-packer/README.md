# sprite-packer

Packs a directory of .png sprites into a single power-of-2 spritesheet and generates a C header with named sprite regions.

## Usage

```
sprite-packer <inputDir> [-o outputDir] [-n name] [-h headerDir] [-p prefix]
```

Or drag a folder onto the exe.

| Flag | Description | Default |
|------|-------------|---------|
| `-o` | Output directory for the .png | input directory |
| `-n` | Base name for the .png file (without extension) | input directory name |
| `-h` | Output directory for the .h file | same as `-o` |
| `-p` | Prefix for the enum, table, and header filename | `sprite` |

By default the header is named `sprite-regions.h` and the table is `spriteRegions[n]`.
With `-p foo` they become `foo-regions.h` and `fooRegions[n]` (enum `FooId`, members `FOO_*`).

## What it does

1. Loads all .png files from the input directory (sorted alphabetically), each copied to 32bpp ARGB
2. Packs into the smallest power-of-2 sheet using a shelf algorithm with 1px transparent padding
3. Outputs the spritesheet .png and a C header with `SpriteRegion` coordinates

## Generated header

The header contains:
- An `enum SpriteId` with entries derived from filenames (uppercased, non-alphanumeric → `_`, prefixed with `SPRITE_`)
- A `static const SpriteRegion spriteRegions[n]` table indexed by the enum
- A `#define SPRITE_FULL ((SpriteRegion){0})` for full-texture references

## Example

```
sprite-packer sprites/ -o res/ -h include/
```

Produces `res/sprites.png` and `include/sprite-regions.h`.
