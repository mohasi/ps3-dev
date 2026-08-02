using System.Windows;

namespace PatchStudio
{
   public partial class App : Application
   {
      protected override void OnStartup(StartupEventArgs e)
      {
         base.OnStartup(e);
         AppSettings.Load();
         var window = new MainWindow();
         if (e.Args.Length > 0) window.OpenPackage(e.Args[0]);   // optional: open a .patchproj on launch
         window.Show();
      }
   }
}
