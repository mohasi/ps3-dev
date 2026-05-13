using System;
using System.Collections.Generic;
using System.Drawing;

namespace SpritePacker
{
    // packs sprites into the smallest power-of-2 sheet using a simple shelf algorithm.
    // sorts sprites by descending height, then places them left-to-right on shelves.
    // adds 1px padding between sprites to prevent texture bleeding with linear filtering.
    internal static class Packer
    {
        private const int Padding = 1;

        public static Size Pack(List<Sprite> sprites)
        {
            // sort tallest first for better shelf utilization
            sprites.Sort((a, b) =>
            {
                int cmp = b.Height.CompareTo(a.Height);
                return cmp != 0 ? cmp : b.Width.CompareTo(a.Width);
            });

            // try increasing power-of-2 sizes until everything fits
            for (int size = 16; size <= 8192; size *= 2)
            {
                if (TryFit(sprites, size, size / 2) && size / 2 >= MinHeight(sprites))
                    return new Size(size, size / 2);
                if (TryFit(sprites, size, size))
                    return new Size(size, size);
            }

            throw new InvalidOperationException("sprites too large to fit in 8192x8192");
        }

        private static bool TryFit(List<Sprite> sprites, int sheetW, int sheetH)
        {
            int shelfX = 0;
            int shelfY = 0;
            int shelfHeight = 0;

            foreach (Sprite s in sprites)
            {
                // new shelf if sprite doesn't fit on current row
                if (shelfX + s.Width > sheetW)
                {
                    shelfY += shelfHeight + Padding;
                    shelfX = 0;
                    shelfHeight = 0;
                }

                // doesn't fit vertically
                if (shelfY + s.Height > sheetH)
                    return false;

                s.X = shelfX;
                s.Y = shelfY;

                shelfX += s.Width + Padding;
                if (s.Height > shelfHeight) shelfHeight = s.Height;
            }

            return true;
        }

        private static int MinHeight(List<Sprite> sprites)
        {
            int max = 0;
            foreach (Sprite s in sprites)
                if (s.Height > max) max = s.Height;
            return max;
        }
    }
}
