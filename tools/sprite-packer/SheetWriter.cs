using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;

namespace SpritePacker
{
    // composes packed sprites onto a single sheet and saves as png
    internal static class SheetWriter
    {
        public static void Write(List<Sprite> sprites, int width, int height, string path)
        {
            using (Bitmap sheet = new Bitmap(width, height, PixelFormat.Format32bppArgb))
            {
                using (Graphics g = Graphics.FromImage(sheet))
                {
                    g.Clear(Color.Transparent);
                    foreach (Sprite s in sprites)
                        g.DrawImage(s.Image, s.X, s.Y, s.Width, s.Height);
                }

                // padding between sprites is left transparent (cleared above) so
                // linear filtering at sprite edges never samples a neighbour colour.
                sheet.Save(path, ImageFormat.Png);
            }
        }
    }
}
