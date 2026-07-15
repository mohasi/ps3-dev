using System;
using System.Windows;

namespace CellStreamServer
{
   public partial class App : Application
   {
      // Windows starts us at log-in with this, so we go straight to the tray rather than putting a
      // window in the user's face on every boot
      public const string StartMinimizedSwitch = "-minimized";

      protected override void OnStartup(StartupEventArgs e)
      {
         base.OnStartup(e);
         AppDomain.CurrentDomain.UnhandledException += (sender, args) => Log.Write("crashed: " + args.ExceptionObject);

         if (!Server.Start())
         {
            MessageBox.Show("Another copy of the server is already running.", "Cell Stream Server",
                            MessageBoxButton.OK, MessageBoxImage.Warning);
            Shutdown();
            return;
         }

         bool startMinimized = Array.IndexOf(e.Args, StartMinimizedSwitch) >= 0;
         var window = new MainWindow();
         if (startMinimized) window.HideToTray();   // the tray icon is created by the window either way
         else window.Show();
      }

      protected override void OnExit(ExitEventArgs e)
      {
         Server.Shutdown();   // puts the desktop resolution back
         base.OnExit(e);
      }
   }
}
