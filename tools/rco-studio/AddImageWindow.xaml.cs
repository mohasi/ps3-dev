using System;
using System.Collections.Generic;
using System.Windows;

namespace RcoStudio
{
   // asks which console format a png being added to an rco should be stored as.
   // defaults to full colour: a new image should never lose quality by surprise.
   public partial class AddImageWindow : Window
   {
      private class FormatChoice
      {
         public ToolRunner.GimFormat Format;
         public string Label;
         public override string ToString() { return Label; }
      }

      public ToolRunner.GimFormat SelectedFormat
      {
         get { return ((FormatChoice)formatList.SelectedItem).Format; }
      }

      public AddImageWindow(Window owner, string rcoName, List<string> pngFiles, string formatsInUse)
      {
         InitializeComponent();
         Owner = owner;

         summaryText.Text = pngFiles.Count == 1
            ? "Adding " + System.IO.Path.GetFileName(pngFiles[0]) + " to " + rcoName + "."
            : "Adding " + pngFiles.Count + " images to " + rcoName + ".";

         foreach (ToolRunner.GimFormat format in Enum.GetValues(typeof(ToolRunner.GimFormat)))
         {
            var choice = new FormatChoice { Format = format, Label = GetFormatLabel(format) };
            formatList.Items.Add(choice);
            if (format == ToolRunner.GimFormat.Rgba8888) formatList.SelectedItem = choice;
         }

         hintText.Text = "This RCO's existing images: " + formatsInUse +
                         "\nMatching them usually looks right, but full colour is always safe.";
      }

      private static string GetFormatLabel(ToolRunner.GimFormat format)
      {
         switch (format)
         {
            case ToolRunner.GimFormat.Rgba8888: return "Rgba8888 — full colour (recommended)";
            case ToolRunner.GimFormat.Rgba5650: return "Rgba5650 — 16-bit, no transparency";
            case ToolRunner.GimFormat.Rgba5551: return "Rgba5551 — 16-bit, on/off transparency";
            case ToolRunner.GimFormat.Rgba4444: return "Rgba4444 — 16-bit, fading transparency";
            case ToolRunner.GimFormat.Index4: return "Index4 — 16 colours";
            case ToolRunner.GimFormat.Index8: return "Index8 — 256 colours";
            case ToolRunner.GimFormat.Dxt1: return "Dxt1 — compressed, lossy";
            case ToolRunner.GimFormat.Dxt3: return "Dxt3 — compressed, lossy";
            case ToolRunner.GimFormat.Dxt5: return "Dxt5 — compressed, lossy";
         }
         return format.ToString();
      }

      private void OnAdd(object sender, RoutedEventArgs e)
      {
         if (formatList.SelectedItem == null) return;
         DialogResult = true;
      }
   }
}
