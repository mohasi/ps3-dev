using System;
using System.IO;
using System.Windows.Media.Imaging;

namespace ThemeStudio
{
   // reading user images: their size, to warn about wrong ones (the sdk compilers accept them
   // silently), and the image itself for the icon grid and preview.
   public static class ImageFile
   {
      public static bool TryReadSize(string path, out int width, out int height)
      {
         width = height = 0;
         try {
            using (FileStream stream = File.OpenRead(path)) {
               BitmapFrame frame = BitmapFrame.Create(stream, BitmapCreateOptions.DelayCreation,
                                                      BitmapCacheOption.None);
               width = frame.PixelWidth;
               height = frame.PixelHeight;
               return width > 0 && height > 0;
            }
         } catch (Exception) {
            return false;
         }
      }

      // one decoded picture, remembered against the file it came from
      private struct Remembered
      {
         public BitmapImage Image;
         public DateTime Written;
      }

      private static readonly System.Collections.Generic.Dictionary<string, Remembered> remembered =
         new System.Collections.Generic.Dictionary<string, Remembered>();

      // read fully on load so the editor never holds the user's file open, and never from wpf's own
      // picture cache: a file replaced at the same path would otherwise keep showing its old
      // contents. null when unreadable, which callers render as an empty slot rather than an error.
      //
      // decoding is remembered here instead, against the file's own timestamp, because the preview
      // redraws every icon on the screen for any change at all -- and re-reading twenty pictures
      // and a full-screen background off disk each time is what made it stutter.
      public static BitmapImage Load(string path)
      {
         if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;

         DateTime written = File.GetLastWriteTimeUtc(path);
         Remembered found;
         if (remembered.TryGetValue(path, out found) && found.Written == written) return found.Image;

         try {
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.CreateOptions = BitmapCreateOptions.IgnoreImageCache;
            image.UriSource = new Uri(path);
            image.EndInit();
            image.Freeze();   // shared between the icon grid and the preview, so it must not be edited
            remembered[path] = new Remembered { Image = image, Written = written };
            return image;
         } catch (Exception) {
            return null;
         }
      }
   }
}
