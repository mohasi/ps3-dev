using System.ComponentModel;
using System.Windows;
using System.Windows.Media;

namespace PatchStudio
{
   // one texture in the open dump. identity is the content hash (stable across reboots), so an
   // edit made against this hash re-applies on the console whenever the same original loads.
   public class TextureItem : INotifyPropertyChanged
   {
      public string Hash { get; set; }        // 8-hex content hash = the dump filename stem
      public int Format { get; set; }          // CellGcmTexture format byte (0x88 = DXT5, ...)
      public int Width { get; set; }
      public int Height { get; set; }
      public int Mips { get; set; }
      public int Size { get; set; }            // bytes of the full blob (base + mips)
      public string BinPath { get; set; }      // local path to <hash>.bin

      public ImageSource Original { get; set; }   // the decoded dump texture; the edit base and the change-comparison reference

      private ImageSource thumbnail;
      public ImageSource Thumbnail                 // what the gallery shows: the edit if there is one, else the original
      {
         get { return thumbnail; }
         set { if (ReferenceEquals(thumbnail, value)) return; thumbnail = value; OnChanged("Thumbnail"); }
      }

      private bool edited;
      public bool Edited                        // true once the edited PNG's pixels differ from the original
      {
         get { return edited; }
         set { if (edited == value) return; edited = value; OnChanged("Edited"); OnChanged("EditedVisibility"); }
      }

      public Visibility EditedVisibility { get { return edited ? Visibility.Visible : Visibility.Collapsed; } }

      public string EditPath { get; set; }      // edited/<hash>.png, once the user has opened it for edit

      public string FormatName
      {
         get
         {
            switch (Format & 0x9f)
            {
               case Dxt.Dxt1: return "DXT1";
               case Dxt.Dxt3: return "DXT3";
               case Dxt.Dxt5: return "DXT5";
               case Dxt.A8R8G8B8: return "ARGB";
               default: return "0x" + Format.ToString("x2");
            }
         }
      }

      public string Caption { get { return Width + "x" + Height + "  " + FormatName; } }

      public event PropertyChangedEventHandler PropertyChanged;
      private void OnChanged(string name)
      {
         if (PropertyChanged != null) PropertyChanged(this, new PropertyChangedEventArgs(name));
      }
   }
}
