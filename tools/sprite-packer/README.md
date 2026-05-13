# sprite-packer

Packs a directory of .png sprites into a single power-of-2 spritesheet and generates a C header with named sprite regions.

## Usage

```
sprite-packer <inputDir> [-o outputDir] [-n name] [-h headerDir]
```

Or drag a folder onto the exe.

| Flag | Description | Default |
|------|-------------|---------|
| `-o` | Output directory for the .png | input directory |
| `-n` | Base name for the .png file (without extension) | input directory name |
| `-h` | Output directory for the .h file | same as `-o` |

The header is always named `sprite-regions.h` and the array is always `spriteRegions[n]`.

## What it does

1. Loads all .png files from the input directory (sorted alphabetically)
2. Packs into the smallest power-of-2 sheet using a shelf algorithm with 1px padding
3. Draws a 1px green border around each sprite region (debug visibility)
4. Outputs the spritesheet .png and a C header (`sprite-regions.h`) with `SpriteRegion` coordinates

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
