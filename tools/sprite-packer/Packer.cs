using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;

namespace SpritePacker
{
   // packs sprites into the smallest power-of-2 sheet with a shelf algorithm:
   // sort tallest-first, then lay sprites left-to-right in rows ("shelves"). a 1px
   // gap between sprites stops linear filtering from bleeding a neighbour's pixels.
   internal static class Packer
   {
      private const int Padding = 1;

      public static Size Pack(List<Sprite> sprites)
      {
         // tallest first, widest breaking ties, for tighter shelves.
         sprites.Sort((left, right) =>
         {
            int byHeight = right.Height.CompareTo(left.Height);
            return byHeight != 0 ? byHeight : right.Width.CompareTo(left.Width);
         });

         int tallest = sprites.Max(sprite => sprite.Height);

         // grow through power-of-2 sizes; prefer a half-height (wide) sheet when it fits.
         for (int size = 16; size <= 8192; size *= 2)
         {
            if (size / 2 >= tallest && Fits(sprites, size, size / 2))
               return new Size(size, size / 2);
            if (Fits(sprites, size, size))
               return new Size(size, size);
         }

         throw new InvalidOperationException("sprites too large to fit in 8192x8192");
      }

      // assigns each sprite an X/Y on the sheet; returns false if they don't all fit.
      private static bool Fits(List<Sprite> sprites, int sheetWidth, int sheetHeight)
      {
         int shelfX = 0, shelfY = 0, shelfHeight = 0;
         foreach (Sprite sprite in sprites)
         {
            // wrap to a new shelf when the sprite overruns the current row.
            if (shelfX + sprite.Width > sheetWidth)
            {
               shelfY += shelfHeight + Padding;
               shelfX = 0;
               shelfHeight = 0;
            }

            if (shelfY + sprite.Height > sheetHeight)
               return false;

            sprite.X = shelfX;
            sprite.Y = shelfY;
            shelfX += sprite.Width + Padding;
            shelfHeight = Math.Max(shelfHeight, sprite.Height);
         }
         return true;
      }
   }
}
