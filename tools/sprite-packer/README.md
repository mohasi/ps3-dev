# sprite-packer

Packs a directory of .png sprites into a single power-of-2 spritesheet and generates a C header with named sprite regions.

## Usage

sprite-packer <inputDir> [-o outputDir] [-n name] [-c headerPath]

Or drag a folder onto the exe.

-o  Output directory for the .png (default: input directory)
-n  Base name for .png and .h files (default: spritesheet)
-c  Override full path for the .h output (default: same dir as .png)

## What it does

1. Loads all .png files from the input directory
2. Trims transparent borders from each sprite
3. Packs into the smallest power-of-2 sheet using a shelf algorithm
4. Outputs the spritesheet image and a C header with SpriteRegion coordinates

Enum names are derived from filenames: uppercased, non-alphanumeric replaced with _, prefixed with SPRITE_.
