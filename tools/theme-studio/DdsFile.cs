using System;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ThemeStudio
{
   // decodes .dds textures so the scene preview can show real surfaces. without this the
   // models render plain grey, which reads as "my model is wrong" when it is only untextured.
   // handles the block-compressed formats the console uses (DXT1/3/5) and plain 32-bit.
   public static class DdsFile
   {
      private const int MagicSize = 4;
      private const int HeaderSize = 124;

      public static BitmapSource Load(string path)
      {
         try {
            byte[] bytes = File.ReadAllBytes(path);
            if (bytes.Length < MagicSize + HeaderSize) return null;
            if (bytes[0] != 'D' || bytes[1] != 'D' || bytes[2] != 'S' || bytes[3] != ' ') return null;

            int height = readInt(bytes, 12);
            int width = readInt(bytes, 16);
            string fourCharCode = readText(bytes, 84);
            int pixelDataStart = MagicSize + HeaderSize;
            if (width <= 0 || height <= 0) return null;

            byte[] pixels;
            switch (fourCharCode) {
               case "DXT1": pixels = decodeBlocks(bytes, pixelDataStart, width, height, 8, false); break;
               case "DXT3": pixels = decodeBlocks(bytes, pixelDataStart, width, height, 16, true); break;
               case "DXT5": pixels = decodeBlocks(bytes, pixelDataStart, width, height, 16, true); break;
               default: pixels = copyUncompressed(bytes, pixelDataStart, width, height); break;
            }
            if (pixels == null) return null;

            var bitmap = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgra32, null,
                                             pixels, width * 4);
            bitmap.Freeze();
            return bitmap;
         } catch (Exception) {
            return null;
         }
      }

      // walks the 4x4 blocks the compressed formats are built from
      private static byte[] decodeBlocks(byte[] bytes, int start, int width, int height,
                                         int blockBytes, bool hasAlphaBlock)
      {
         var pixels = new byte[width * height * 4];
         int blocksAcross = (width + 3) / 4;
         int blocksDown = (height + 3) / 4;
         int at = start;

         for (int blockY = 0; blockY < blocksDown; blockY++) {
            for (int blockX = 0; blockX < blocksAcross; blockX++) {
               if (at + blockBytes > bytes.Length) return pixels;
               int colourAt = hasAlphaBlock ? at + 8 : at;
               writeBlock(bytes, colourAt, pixels, width, height, blockX * 4, blockY * 4, blockBytes == 8);
               at += blockBytes;
            }
         }
         return pixels;
      }

      // one 4x4 colour block: two endpoint colours, then two bits per pixel choosing between them
      private static void writeBlock(byte[] bytes, int at, byte[] pixels, int width, int height,
                                     int originX, int originY, bool mayBeTransparent)
      {
         int first = bytes[at] | (bytes[at + 1] << 8);
         int second = bytes[at + 2] | (bytes[at + 3] << 8);
         var red = new byte[4]; var green = new byte[4]; var blue = new byte[4]; var alpha = new byte[4];

         unpack565(first, out red[0], out green[0], out blue[0]);
         unpack565(second, out red[1], out green[1], out blue[1]);
         alpha[0] = alpha[1] = alpha[2] = alpha[3] = 255;

         if (first > second || !mayBeTransparent) {
            // four shades: the two endpoints and two blends
            for (int channel = 0; channel < 3; channel++) {
               byte[] table = channel == 0 ? red : channel == 1 ? green : blue;
               table[2] = (byte)((2 * table[0] + table[1]) / 3);
               table[3] = (byte)((table[0] + 2 * table[1]) / 3);
            }
         } else {
            // three shades plus a fully transparent slot
            for (int channel = 0; channel < 3; channel++) {
               byte[] table = channel == 0 ? red : channel == 1 ? green : blue;
               table[2] = (byte)((table[0] + table[1]) / 2);
               table[3] = 0;
            }
            alpha[3] = 0;
         }

         uint indices = (uint)(bytes[at + 4] | (bytes[at + 5] << 8) | (bytes[at + 6] << 16) |
                               (bytes[at + 7] << 24));
         for (int row = 0; row < 4; row++) {
            for (int column = 0; column < 4; column++) {
               int x = originX + column, y = originY + row;
               if (x >= width || y >= height) continue;
               int choice = (int)((indices >> (2 * (row * 4 + column))) & 3);
               int target = (y * width + x) * 4;
               pixels[target] = blue[choice];
               pixels[target + 1] = green[choice];
               pixels[target + 2] = red[choice];
               pixels[target + 3] = alpha[choice];
            }
         }
      }

      private static void unpack565(int packed, out byte red, out byte green, out byte blue)
      {
         red = (byte)(((packed >> 11) & 0x1F) * 255 / 31);
         green = (byte)(((packed >> 5) & 0x3F) * 255 / 63);
         blue = (byte)((packed & 0x1F) * 255 / 31);
      }

      // uncompressed textures are already 32-bit; anything else is not worth guessing at
      private static byte[] copyUncompressed(byte[] bytes, int start, int width, int height)
      {
         int needed = width * height * 4;
         if (start + needed > bytes.Length) return null;
         var pixels = new byte[needed];
         Array.Copy(bytes, start, pixels, 0, needed);
         return pixels;
      }

      private static int readInt(byte[] bytes, int offset)
      {
         return bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) |
                (bytes[offset + 3] << 24);
      }

      private static string readText(byte[] bytes, int offset)
      {
         return "" + (char)bytes[offset] + (char)bytes[offset + 1] +
                     (char)bytes[offset + 2] + (char)bytes[offset + 3];
      }
   }
}
