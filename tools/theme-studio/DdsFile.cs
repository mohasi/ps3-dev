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
               case "DXT1": pixels = decodeBlocks(bytes, pixelDataStart, width, height, AlphaKind.None); break;
               case "DXT3": pixels = decodeBlocks(bytes, pixelDataStart, width, height, AlphaKind.Stepped); break;
               case "DXT5": pixels = decodeBlocks(bytes, pixelDataStart, width, height, AlphaKind.Blended); break;
               default: pixels = copyUncompressed(bytes, pixelDataStart, width, height, readInt(bytes, 104)); break;
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

      // how the format stores transparency. DXT1 has only the one see-through slot in its colour
      // block; DXT3 keeps a plain number per pixel; DXT5 keeps two ends and blends between them.
      private enum AlphaKind { None, Stepped, Blended }

      // walks the 4x4 blocks the compressed formats are built from
      private static byte[] decodeBlocks(byte[] bytes, int start, int width, int height, AlphaKind alphaKind)
      {
         var pixels = new byte[width * height * 4];
         int blockBytes = alphaKind == AlphaKind.None ? 8 : 16;
         int blocksAcross = (width + 3) / 4;
         int blocksDown = (height + 3) / 4;
         var alpha = new byte[16];
         int at = start;

         for (int blockY = 0; blockY < blocksDown; blockY++) {
            for (int blockX = 0; blockX < blocksAcross; blockX++) {
               if (at + blockBytes > bytes.Length) return pixels;
               readAlphaBlock(bytes, at, alphaKind, alpha);
               int colourAt = alphaKind == AlphaKind.None ? at : at + 8;
               writeBlock(bytes, colourAt, pixels, width, height, blockX * 4, blockY * 4,
                          alphaKind == AlphaKind.None, alpha);
               at += blockBytes;
            }
         }
         return pixels;
      }

      // fills in the 16 pixels' transparency for one block, ahead of the colour
      private static void readAlphaBlock(byte[] bytes, int at, AlphaKind alphaKind, byte[] alpha)
      {
         if (alphaKind == AlphaKind.None) {
            for (int pixel = 0; pixel < 16; pixel++) alpha[pixel] = 255;
            return;
         }

         if (alphaKind == AlphaKind.Stepped) {
            // four bits per pixel, two pixels per byte, low half first
            for (int pixel = 0; pixel < 16; pixel++) {
               int half = (bytes[at + pixel / 2] >> (pixel % 2 == 0 ? 0 : 4)) & 0xF;
               alpha[pixel] = (byte)(half * 17);   // 0..15 spread over 0..255
            }
            return;
         }

         // two end values, then three bits per pixel choosing between them or a blend
         var alphaRamp = new byte[8];
         alphaRamp[0] = bytes[at];
         alphaRamp[1] = bytes[at + 1];
         if (alphaRamp[0] > alphaRamp[1]) {
            for (int step = 1; step <= 6; step++)
               alphaRamp[step + 1] = (byte)(((7 - step) * alphaRamp[0] + step * alphaRamp[1]) / 7);
         } else {
            for (int step = 1; step <= 4; step++)
               alphaRamp[step + 1] = (byte)(((5 - step) * alphaRamp[0] + step * alphaRamp[1]) / 5);
            alphaRamp[6] = 0;
            alphaRamp[7] = 255;
         }

         long choices = 0;
         for (int index = 0; index < 6; index++) choices |= (long)bytes[at + 2 + index] << (8 * index);
         for (int pixel = 0; pixel < 16; pixel++)
            alpha[pixel] = alphaRamp[(int)((choices >> (3 * pixel)) & 7)];
      }

      // one 4x4 colour block: two endpoint colours, then two bits per pixel choosing between them
      private static void writeBlock(byte[] bytes, int at, byte[] pixels, int width, int height,
                                     int originX, int originY, bool mayBeTransparent, byte[] alpha)
      {
         int first = bytes[at] | (bytes[at + 1] << 8);
         int second = bytes[at + 2] | (bytes[at + 3] << 8);
         var red = new byte[4]; var green = new byte[4]; var blue = new byte[4];
         bool hasTransparentSlot = mayBeTransparent && first <= second;

         unpack565(first, out red[0], out green[0], out blue[0]);
         unpack565(second, out red[1], out green[1], out blue[1]);

         if (!hasTransparentSlot) {
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
         }

         uint indices = (uint)(bytes[at + 4] | (bytes[at + 5] << 8) | (bytes[at + 6] << 16) |
                               (bytes[at + 7] << 24));
         for (int row = 0; row < 4; row++) {
            for (int column = 0; column < 4; column++) {
               int x = originX + column, y = originY + row;
               if (x >= width || y >= height) continue;
               int pixel = row * 4 + column;
               int choice = (int)((indices >> (2 * pixel)) & 3);
               int target = (y * width + x) * 4;
               pixels[target] = blue[choice];
               pixels[target + 1] = green[choice];
               pixels[target + 2] = red[choice];
               pixels[target + 3] = hasTransparentSlot && choice == 3 ? (byte)0 : alpha[pixel];
            }
         }
      }

      private static void unpack565(int packed, out byte red, out byte green, out byte blue)
      {
         red = (byte)(((packed >> 11) & 0x1F) * 255 / 31);
         green = (byte)(((packed >> 5) & 0x3F) * 255 / 63);
         blue = (byte)((packed & 0x1F) * 255 / 31);
      }

      // uncompressed textures are already 32-bit; anything else is not worth guessing at.
      // a file with no transparency channel leaves that byte at zero, which would read as a
      // fully see-through texture, so those are forced solid instead.
      private static byte[] copyUncompressed(byte[] bytes, int start, int width, int height, int alphaMask)
      {
         int needed = width * height * 4;
         if (start + needed > bytes.Length) return null;
         var pixels = new byte[needed];
         Array.Copy(bytes, start, pixels, 0, needed);
         if (alphaMask == 0)
            for (int target = 3; target < needed; target += 4) pixels[target] = 255;
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
