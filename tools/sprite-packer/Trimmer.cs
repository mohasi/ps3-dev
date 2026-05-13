using System.Drawing;
using System.Drawing.Imaging;

namespace SpritePacker
{
    // returns a copy of the source bitmap without modification
    internal static class Trimmer
    {
        public static Bitmap Copy(Bitmap src)
        {
            Bitmap result = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb);
            using (Graphics g = Graphics.FromImage(result))
                g.DrawImage(src, 0, 0, src.Width, src.Height);
            return result;
        }
    }
}
