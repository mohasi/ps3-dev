using System.Collections.Generic;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace PatchStudio
{
   // turns the edited textures into a patch: only the ones the user changed, each re-encoded to its
   // own format + mip chain and keyed by content hash. unchanged textures are never touched, so the
   // console keeps them exactly as the game shipped them. mirrors the plugin's apply manifest:
   //   "origHash replHash width height fmt file"  (origHash = live match, replHash = already-applied)
   public static class PatchBuilder
   {
      public class Result
      {
         public int Included;                                  // textures written to the patch
         public List<string> Skipped = new List<string>();     // hash + reason, surfaced to the user
      }

      public static uint Fnv1a(byte[] data)
      {
         uint hash = 2166136261u;
         foreach (byte b in data) { hash ^= b; hash *= 16777619u; }
         return hash;
      }

      public static Result Build(DumpProject project, string outFolder)
      {
         Directory.CreateDirectory(outFolder);
         var result = new Result();
         var manifest = new List<string> { "# origHash replHash width height fmt file" };
         string editDir = Path.Combine(project.Folder, "edited");

         foreach (TextureItem item in project.Textures)
         {
            if (!item.Edited) continue;
            string editPng = Path.Combine(editDir, item.Hash + ".png");
            if (!File.Exists(editPng) || !File.Exists(item.BinPath)) continue;

            int pw, ph;
            byte[] bgra = LoadBgra(editPng, out pw, out ph);
            if (bgra == null) { result.Skipped.Add(item.Hash + " (can't read edit)"); continue; }
            if (pw != item.Width || ph != item.Height)
            {
               result.Skipped.Add(item.Hash + " (edit is " + pw + "x" + ph + ", must stay " + item.Width + "x" + item.Height + ")");
               continue;
            }

            byte[] original = File.ReadAllBytes(item.BinPath);
            byte[] replacement = Dxt.Encode(bgra, item.Width, item.Height, item.Format, item.Mips);
            if (replacement.Length != original.Length)
            {
               result.Skipped.Add(item.Hash + " (encoded size mismatch)");   // shouldn't happen — same dims/format/mips
               continue;
            }

            uint origHash = Fnv1a(original), replHash = Fnv1a(replacement);
            if (replHash == origHash) { result.Skipped.Add(item.Hash + " (no change after encode)"); continue; }

            string file = item.Hash + ".bin";
            File.WriteAllBytes(Path.Combine(outFolder, file), replacement);
            manifest.Add(string.Format("{0:x8} {1:x8} {2} {3} {4:x2} {5}", origHash, replHash, item.Width, item.Height, item.Format, file));
            result.Included++;
         }

         File.WriteAllLines(Path.Combine(outFolder, "manifest.txt"), manifest);
         return result;
      }

      // load an edited PNG as BGRA32 at its own pixel size (the caller checks it matches the original).
      private static byte[] LoadBgra(string path, out int width, out int height)
      {
         width = height = 0;
         try
         {
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.CreateOptions = BitmapCreateOptions.IgnoreImageCache;   // encode the current edit, not a stale cached decode
            image.UriSource = new System.Uri(path);
            image.EndInit();
            var bgra = new FormatConvertedBitmap(image, PixelFormats.Bgra32, null, 0);
            width = bgra.PixelWidth; height = bgra.PixelHeight;
            int stride = width * 4;
            byte[] pixels = new byte[stride * height];
            bgra.CopyPixels(pixels, stride, 0);
            return pixels;
         }
         catch { return null; }
      }
   }
}
