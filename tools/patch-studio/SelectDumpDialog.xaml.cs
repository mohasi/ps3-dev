using System.Collections.Generic;
using System.Windows;
using System.Windows.Input;

namespace PatchStudio
{
   // pick which game's dump folder to pull (the title-id subfolders under /dumps on the console)
   public partial class SelectDumpDialog : Window
   {
      public string Selected { get { return list.SelectedItem as string; } }

      public SelectDumpDialog(List<string> titleIds, string preselect)
      {
         InitializeComponent();
         list.ItemsSource = titleIds;
         list.SelectedItem = preselect != null && titleIds.Contains(preselect) ? preselect : (titleIds.Count > 0 ? titleIds[0] : null);
      }

      private void Ok_Click(object sender, RoutedEventArgs e) { Confirm(); }
      private void List_DoubleClick(object sender, MouseButtonEventArgs e) { Confirm(); }

      private void Confirm()
      {
         if (Selected == null) return;
         DialogResult = true;
      }

      private void Cancel_Click(object sender, RoutedEventArgs e) { DialogResult = false; }
   }
}
