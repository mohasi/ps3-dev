using System.Drawing;

namespace SpritePacker
{
   // a single named sprite with its trimmed image and packed position
   internal sealed class Sprite
   {
      public string Name { get; private set; }
      public Bitmap Image { get; private set; }
      public int X { get; set; }
      public int Y { get; set; }

      public int Width { get { return Image.Width; } }
      public int Height { get { return Image.Height; } }

      public Sprite(string name, Bitmap image)
      {
         Name = name;
         Image = image;
      }
   }
}
