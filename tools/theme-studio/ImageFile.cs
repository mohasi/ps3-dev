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

      // read fully on load so the editor never holds the user's file open, and never from the
      // picture cache: a file replaced at the same path would otherwise keep showing its old
      // contents. null when unreadable, which callers render as an empty slot rather than an error.
      public static BitmapImage Load(string path)
      {
         if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
         try {
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.CreateOptions = BitmapCreateOptions.IgnoreImageCache;
            image.UriSource = new Uri(path);
            image.EndInit();
            return image;
         } catch (Exception) {
            return null;
         }
      }
   }
}
