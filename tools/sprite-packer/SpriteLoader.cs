using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;

namespace SpritePacker
{
    // loads and trims all .png files from a directory
    internal static class SpriteLoader
    {
        public static List<Sprite> Load(string dir)
        {
            string[] files = Directory.GetFiles(dir, "*.png");
            Array.Sort(files, StringComparer.OrdinalIgnoreCase);

            List<Sprite> sprites = new List<Sprite>();
            foreach (string file in files)
            {
                string name = Path.GetFileNameWithoutExtension(file);
                using (Bitmap raw = new Bitmap(file))
                {
                    Bitmap copy = Trimmer.Copy(raw);
                    sprites.Add(new Sprite(name, copy));
                }
            }
            return sprites;
        }
    }
}
