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

                // draw green 1px border around each sprite in the padding area
                Color green = Color.FromArgb(255, 0, 255, 0);
                foreach (Sprite s in sprites)
                {
                    int left = s.X - 1;
                    int top = s.Y - 1;
                    int right = s.X + s.Width;
                    int bottom = s.Y + s.Height;

                    for (int x = left; x <= right; x++)
                    {
                        if (x >= 0 && x < width)
                        {
                            if (top >= 0) sheet.SetPixel(x, top, green);
                            if (bottom < height) sheet.SetPixel(x, bottom, green);
                        }
                    }
                    for (int y = top; y <= bottom; y++)
                    {
                        if (y >= 0 && y < height)
                        {
                            if (left >= 0) sheet.SetPixel(left, y, green);
                            if (right < width) sheet.SetPixel(right, y, green);
                        }
                    }
                }

                sheet.Save(path, ImageFormat.Png);
            }
        }
    }
}
