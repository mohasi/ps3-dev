using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;

namespace SpritePacker
{
   // loads every .png in a directory (alphabetical order) as a 32bpp argb sprite.
   internal static class SpriteLoader
   {
     public static List<Sprite> Load(string directory)
     {
       string[] files = Directory.GetFiles(directory, "*.png");
       Array.Sort(files, StringComparer.OrdinalIgnoreCase);

       var sprites = new List<Sprite>(files.Length);
       foreach (string file in files)
       {
         string name = Path.GetFileNameWithoutExtension(file);
         using (var source = new Bitmap(file))
            sprites.Add(new Sprite(name, ToArgb32(source)));
       }
       return sprites;
     }

     // copies any source bitmap into a fresh 32bpp argb bitmap, so every sprite
     // shares one pixel format when it is composited onto the sheet.
     private static Bitmap ToArgb32(Bitmap source)
     {
       var copy = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb);
       using (Graphics graphics = Graphics.FromImage(copy))
         graphics.DrawImage(source, 0, 0, source.Width, source.Height);
       return copy;
     }
   }
}
