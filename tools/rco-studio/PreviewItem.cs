using System.Windows.Media;

namespace RcoStudio
{
   // one tile in the preview pane: an image thumbnail, a sound, or a text/xml hit
   public class PreviewItem
   {
      public string FilePath { get; set; }
      public string Caption { get; set; }
      public string ToolTipText { get; set; }
      public ImageSource Thumbnail { get; set; }   // null for sounds and text hits
      public string Glyph { get; set; }            // Segoe MDL2 icon shown when there is no thumbnail
      public bool IsSound { get; set; }
      public bool IsEdited { get; set; }           // amber border + pencil badge on the tile
      public bool IsLossy { get; set; }            // DXT image / VAG sound: editing re-encodes lossily
   }

   // one label/value line in the properties panel beside the preview
   public class PropertyRow
   {
      public string Label { get; set; }
      public string Value { get; set; }

      public PropertyRow(string label, string value) { Label = label; Value = value; }
   }
}
