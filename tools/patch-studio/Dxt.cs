using System;

namespace PatchStudio
{
   // decode PS3 texture formats to BGRA32 (what WPF's BitmapSource wants). the console stores DXT
   // block-linear with no swizzle (confirmed on hardware), so standard BC1/2/3 block order applies.
   // base mip level only — that's all a thumbnail or an edit needs.
   public static class Dxt
   {
      // the format byte from a CellGcmTexture, with the LN (0x20) / UN (0x40) flags stripped.
      public const int A8R8G8B8 = 0x85;
      public const int Dxt1     = 0x86;
      public const int Dxt3     = 0x87;
      public const int Dxt5     = 0x88;

      public static bool CanDecode(int format)
      {
         int b = format & 0x9f;
         return b == A8R8G8B8 || b == Dxt1 || b == Dxt3 || b == Dxt5;
      }

      // returns BGRA32 pixels (width*height*4) or null if the format isn't handled.
      public static byte[] Decode(byte[] data, int width, int height, int format)
      {
         switch (format & 0x9f)
         {
            case A8R8G8B8: return DecodeArgb(data, width, height);
            case Dxt1:     return DecodeBlocks(data, width, height, 8, false, false);
            case Dxt3:     return DecodeBlocks(data, width, height, 16, true, false);
            case Dxt5:     return DecodeBlocks(data, width, height, 16, false, true);
            default:       return null;
         }
      }

      private static byte[] DecodeArgb(byte[] data, int width, int height)
      {
         // stored A8R8G8B8 big-endian; WPF wants BGRA byte order
         byte[] outp = new byte[width * height * 4];
         for (int i = 0; i < width * height && (i * 4 + 3) < data.Length; i++)
         {
            byte a = data[i * 4 + 0], r = data[i * 4 + 1], g = data[i * 4 + 2], b = data[i * 4 + 3];
            outp[i * 4 + 0] = b; outp[i * 4 + 1] = g; outp[i * 4 + 2] = r; outp[i * 4 + 3] = a;
         }
         return outp;
      }

      // one path for BC1/2/3: colour block is identical, only the alpha source differs.
      private static byte[] DecodeBlocks(byte[] data, int width, int height, int blockBytes, bool explicitAlpha, bool interpAlpha)
      {
         byte[] outp = new byte[width * height * 4];
         int blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
         int p = 0;

         for (int by = 0; by < blocksHigh; by++)
         {
            for (int bx = 0; bx < blocksWide; bx++)
            {
               if (p + blockBytes > data.Length) return outp;
               int colourOffset = p + (blockBytes - 8);

               // alpha table for the 16 texels
               byte[] alpha = new byte[16];
               if (interpAlpha) DecodeInterpAlpha(data, p, alpha);
               else if (explicitAlpha) DecodeExplicitAlpha(data, p, alpha);
               else for (int i = 0; i < 16; i++) alpha[i] = 255;

               // colour endpoints (RGB565) + 2-bit indices
               int c0 = data[colourOffset] | (data[colourOffset + 1] << 8);
               int c1 = data[colourOffset + 2] | (data[colourOffset + 3] << 8);
               int[][] palette = BuildColourPalette(c0, c1, blockBytes == 8);
               uint bits = (uint)(data[colourOffset + 4] | (data[colourOffset + 5] << 8) | (data[colourOffset + 6] << 16) | (data[colourOffset + 7] << 24));

               for (int ty = 0; ty < 4; ty++)
               {
                  for (int tx = 0; tx < 4; tx++)
                  {
                     int px = bx * 4 + tx, py = by * 4 + ty;
                     if (px >= width || py >= height) continue;
                     int texel = ty * 4 + tx;
                     int ci = (int)((bits >> (2 * texel)) & 3);
                     int[] rgb = palette[ci];
                     int o = (py * width + px) * 4;
                     bool punch = blockBytes == 8 && c0 <= c1 && ci == 3;   // BC1 1-bit alpha
                     outp[o + 0] = (byte)rgb[2]; outp[o + 1] = (byte)rgb[1]; outp[o + 2] = (byte)rgb[0];
                     outp[o + 3] = punch ? (byte)0 : alpha[texel];
                  }
               }
               p += blockBytes;
            }
         }
         return outp;
      }

      private static int[][] BuildColourPalette(int c0, int c1, bool bc1)
      {
         int[] e0 = From565(c0), e1 = From565(c1);
         int[][] pal = new int[4][];
         pal[0] = e0; pal[1] = e1;
         if (bc1 && c0 <= c1)
         {
            pal[2] = new[] { (e0[0] + e1[0]) / 2, (e0[1] + e1[1]) / 2, (e0[2] + e1[2]) / 2 };
            pal[3] = new[] { 0, 0, 0 };   // transparent/black
         }
         else
         {
            pal[2] = new[] { (2 * e0[0] + e1[0]) / 3, (2 * e0[1] + e1[1]) / 3, (2 * e0[2] + e1[2]) / 3 };
            pal[3] = new[] { (e0[0] + 2 * e1[0]) / 3, (e0[1] + 2 * e1[1]) / 3, (e0[2] + 2 * e1[2]) / 3 };
         }
         return pal;
      }

      private static int[] From565(int c)
      {
         int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
         return new[] { r * 255 / 31, g * 255 / 63, b * 255 / 31 };
      }

      private static void DecodeExplicitAlpha(byte[] data, int block, byte[] alpha)
      {
         for (int i = 0; i < 16; i++)
         {
            int nibble = (data[block + i / 2] >> ((i & 1) * 4)) & 0xf;
            alpha[i] = (byte)(nibble * 17);
         }
      }

      private static void DecodeInterpAlpha(byte[] data, int block, byte[] alpha)
      {
         int a0 = data[block], a1 = data[block + 1];
         int[] table = new int[8];
         table[0] = a0; table[1] = a1;
         if (a0 > a1) for (int i = 1; i < 7; i++) table[i + 1] = ((7 - i) * a0 + i * a1) / 7;
         else { for (int i = 1; i < 5; i++) table[i + 1] = ((5 - i) * a0 + i * a1) / 5; table[6] = 0; table[7] = 255; }

         long bits = 0;
         for (int i = 0; i < 6; i++) bits |= (long)data[block + 2 + i] << (8 * i);
         for (int i = 0; i < 16; i++) alpha[i] = (byte)table[(int)((bits >> (3 * i)) & 7)];
      }

      // ---- encode: BGRA32 -> the texture's own format, full mip chain, matching the original blob size ----

      // encode base + (mips-1) downscaled levels, concatenated, so the result is a drop-in for the
      // original texture (same layout, same byte size the plugin overwrites).
      public static byte[] Encode(byte[] bgra, int width, int height, int format, int mips)
      {
         var output = new System.IO.MemoryStream();
         byte[] level = bgra;
         int w = width, h = height;
         for (int m = 0; m < mips; m++)
         {
            byte[] encoded = EncodeLevel(level, w, h, format);
            output.Write(encoded, 0, encoded.Length);
            if (m + 1 < mips) { level = Downscale(level, w, h, out w, out h); }
         }
         return output.ToArray();
      }

      private static byte[] EncodeLevel(byte[] bgra, int width, int height, int format)
      {
         switch (format & 0x9f)
         {
            case A8R8G8B8: return EncodeArgb(bgra, width, height);
            case Dxt1:     return EncodeBlocks(bgra, width, height, 8, false, false);
            case Dxt3:     return EncodeBlocks(bgra, width, height, 16, true, false);
            default:       return EncodeBlocks(bgra, width, height, 16, false, true);   // DXT5
         }
      }

      private static byte[] EncodeArgb(byte[] bgra, int width, int height)
      {
         byte[] outp = new byte[width * height * 4];
         for (int i = 0; i < width * height; i++)
         {
            byte b = bgra[i * 4 + 0], g = bgra[i * 4 + 1], r = bgra[i * 4 + 2], a = bgra[i * 4 + 3];
            outp[i * 4 + 0] = a; outp[i * 4 + 1] = r; outp[i * 4 + 2] = g; outp[i * 4 + 3] = b;
         }
         return outp;
      }

      private static byte[] EncodeBlocks(byte[] bgra, int width, int height, int blockBytes, bool explicitAlpha, bool interpAlpha)
      {
         int blocksWide = (width + 3) / 4, blocksHigh = (height + 3) / 4;
         byte[] outp = new byte[blocksWide * blocksHigh * blockBytes];
         int p = 0;
         int[] r = new int[16], g = new int[16], b = new int[16], a = new int[16];

         for (int by = 0; by < blocksHigh; by++)
         {
            for (int bx = 0; bx < blocksWide; bx++)
            {
               for (int ty = 0; ty < 4; ty++)
                  for (int tx = 0; tx < 4; tx++)
                  {
                     int px = Math.Min(bx * 4 + tx, width - 1), py = Math.Min(by * 4 + ty, height - 1);
                     int o = (py * width + px) * 4, t = ty * 4 + tx;
                     b[t] = bgra[o + 0]; g[t] = bgra[o + 1]; r[t] = bgra[o + 2]; a[t] = bgra[o + 3];
                  }

               if (interpAlpha) EncodeInterpAlpha(a, outp, p);
               else if (explicitAlpha) EncodeExplicitAlpha(a, outp, p);
               EncodeColour(r, g, b, outp, p + (blockBytes - 8), blockBytes == 8);
               p += blockBytes;
            }
         }
         return outp;
      }

      private static void EncodeColour(int[] r, int[] g, int[] b, byte[] outp, int at, bool bc1)
      {
         int rmin = 255, gmin = 255, bmin = 255, rmax = 0, gmax = 0, bmax = 0;
         for (int i = 0; i < 16; i++)
         {
            if (r[i] < rmin) rmin = r[i]; if (r[i] > rmax) rmax = r[i];
            if (g[i] < gmin) gmin = g[i]; if (g[i] > gmax) gmax = g[i];
            if (b[i] < bmin) bmin = b[i]; if (b[i] > bmax) bmax = b[i];
         }
         int c0 = To565(rmax, gmax, bmax), c1 = To565(rmin, gmin, bmin);
         if (c0 < c1) { int tmp = c0; c0 = c1; c1 = tmp; }
         if (c0 == c1) { c1 = c0 > 0 ? c0 - 1 : 0; if (c0 == c1) c0 = 1; }   // keep 4-colour mode

         int[][] palette = BuildColourPalette(c0, c1, false);
         uint bits = 0;
         for (int i = 0; i < 16; i++)
         {
            int best = 0, bestDist = int.MaxValue;
            for (int k = 0; k < 4; k++)
            {
               int dr = palette[k][0] - r[i], dg = palette[k][1] - g[i], db = palette[k][2] - b[i];
               int dist = dr * dr + dg * dg + db * db;
               if (dist < bestDist) { bestDist = dist; best = k; }
            }
            bits |= (uint)best << (2 * i);
         }
         outp[at + 0] = (byte)(c0 & 0xff); outp[at + 1] = (byte)(c0 >> 8);
         outp[at + 2] = (byte)(c1 & 0xff); outp[at + 3] = (byte)(c1 >> 8);
         outp[at + 4] = (byte)(bits & 0xff); outp[at + 5] = (byte)((bits >> 8) & 0xff);
         outp[at + 6] = (byte)((bits >> 16) & 0xff); outp[at + 7] = (byte)((bits >> 24) & 0xff);
      }

      private static void EncodeExplicitAlpha(int[] a, byte[] outp, int at)
      {
         for (int i = 0; i < 8; i++)
            outp[at + i] = (byte)(((a[i * 2] >> 4) & 0xf) | ((a[i * 2 + 1] >> 4) << 4));
      }

      private static void EncodeInterpAlpha(int[] a, byte[] outp, int at)
      {
         int a0 = 0, a1 = 255;
         for (int i = 0; i < 16; i++) { if (a[i] > a0) a0 = a[i]; if (a[i] < a1) a1 = a[i]; }
         if (a0 == a1) { a1 = a0 > 0 ? a0 - 1 : 0; if (a0 == a1) a0 = 1; }
         int[] table = new int[8]; table[0] = a0; table[1] = a1;
         for (int i = 1; i < 7; i++) table[i + 1] = ((7 - i) * a0 + i * a1) / 7;

         outp[at] = (byte)a0; outp[at + 1] = (byte)a1;
         long bits = 0;
         for (int i = 0; i < 16; i++)
         {
            int best = 0, bestDist = int.MaxValue;
            for (int k = 0; k < 8; k++) { int d = table[k] - a[i]; if (d < 0) d = -d; if (d < bestDist) { bestDist = d; best = k; } }
            bits |= (long)best << (3 * i);
         }
         for (int i = 0; i < 6; i++) outp[at + 2 + i] = (byte)((bits >> (8 * i)) & 0xff);
      }

      private static int To565(int r, int g, int b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

      // 2x2 box downscale to the next mip level (dims halve, min 1).
      private static byte[] Downscale(byte[] src, int w, int h, out int nw, out int nh)
      {
         nw = Math.Max(1, w / 2); nh = Math.Max(1, h / 2);
         byte[] dst = new byte[nw * nh * 4];
         for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
               for (int c = 0; c < 4; c++)
               {
                  int x0 = Math.Min(x * 2, w - 1), x1 = Math.Min(x * 2 + 1, w - 1);
                  int y0 = Math.Min(y * 2, h - 1), y1 = Math.Min(y * 2 + 1, h - 1);
                  int sum = src[(y0 * w + x0) * 4 + c] + src[(y0 * w + x1) * 4 + c] + src[(y1 * w + x0) * 4 + c] + src[(y1 * w + x1) * 4 + c];
                  dst[(y * nw + x) * 4 + c] = (byte)(sum / 4);
               }
         return dst;
      }
   }
}
