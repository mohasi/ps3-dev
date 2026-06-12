using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;

namespace SpritePacker
{
   // composites the packed sprites onto one transparent sheet and saves it as png.
   internal static class SheetWriter
   {
      public static void Write(List<Sprite> sprites, int width, int height, string path)
      {
         using (var sheet = new Bitmap(width, height, PixelFormat.Format32bppArgb))
         {
            using (Graphics graphics = Graphics.FromImage(sheet))
            {
               graphics.Clear(Color.Transparent);
               foreach (Sprite sprite in sprites)
                  graphics.DrawImage(sprite.Image, sprite.X, sprite.Y, sprite.Width, sprite.Height);
            }

            // the 1px gaps stay transparent (cleared above) so edge filtering never
            // samples a neighbouring sprite's colour.
            sheet.Save(path, ImageFormat.Png);
         }
      }
   }
}
